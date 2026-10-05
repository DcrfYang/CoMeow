// mgmp_unlocks.cpp -- see mgmp_unlocks.h.
#include "mgmp_unlocks.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "mgmp_checkpoint_io.h"
#include "mgmp_savefile.h"

#include "mgmp_addresses.h"
#include "mgmp_catsync.h"
#include "mgmp_rng.h"
#include "mgmp_log.h"
#include "mgmp_lockstep.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_resolve.h"
#include "mgmp_unlock_lists.h"
#include "mgmp_room.h"
#include "mgmp_tuning.h"
#include "mgmp_event_keys.h"

namespace mgmp {
namespace {

// Every flag a map's `flags {}` block or the progression file names (the generator's own `GenFlag_*` markers are set by the generation itself
// and are left out). The ORDER IS THE WIRE FORMAT.
const char* const kFlagNames[] = {
    "BoneyardUnlocked", "BothObelisksUnlocked", "BunkerUnlocked", "CavesUnlocked", "ChaosAntennaAttached", "CoreObeliskUnlocked",
    "CoreUnlocked", "CraterUnlocked", "DesertUnlocked", "DimensionXUnlocked", "EndOfTimeUnlocked", "FutureUnlocked",
    "HardPathUnlocked", "IceAgeUnlocked", "JunkyardUnlocked", "JurassicUnlocked", "LabUnlocked", "MeatWorldUnlocked",
    "MeatWorldUnlockedFull", "MoonObeliskUnlocked", "MoonUnlocked", "OrbAntennaAttached", "SewersUnlocked", "TheEndUnlocked",
    "ThrobbingArteryDone", "VolcanoAntennaAttached", "WallOfFleshDone",
};
constexpr int kFlagCount = (int)(sizeof(kFlagNames) / sizeof(kFlagNames[0]));
static_assert(kFlagCount <= 32, "the flag set travels in a u32");

// The game's std::string, MSVC layout: 16 bytes of buffer or a heap pointer, size, capacity.
struct GameStr {
    union { char buf[16]; char* ptr; };
    uint64_t size;
    uint64_t cap;
};

// sub_14022C5E0: the property getter takes its key BY VALUE and destroys it (both the cached and the database path end in _Tidy_deallocate),
// so the key's characters have to live on the GAME's heap -- the same arrangement mgmp_invsync makes for its bucket keys.
using fn_alloc    = void*   (__fastcall*)(size_t n);
using fn_prop_int = int64_t (__fastcall*)(void* props, GameStr* key, int64_t fallback);

bool make_key(GameStr& s, const char* text, fn_alloc alloc) {
    const size_t n = strlen(text);
    memset(&s, 0, sizeof(s));
    if (n <= 15) { memcpy(s.buf, text, n + 1); s.size = n; s.cap = 15; return true; }
    size_t cap = n | 15;
    if (cap < 22) cap = 22;
    void* p = nullptr;
    __try { p = alloc(cap + 1); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!p) return false;
    memcpy(p, text, n + 1);
    s.ptr = (char*)p; s.size = n; s.cap = cap;
    return true;
}

bool g_own_read = false;          // this module is reading: the detour must not answer from the override
bool g_override_on = false;
uint32_t g_override = 0;
ULONGLONG g_override_at = 0;
constexpr ULONGLONG kOverrideMaxMs = 60000;   // the map is generated within seconds of the commit; a stuck override must not outlive that

bool read_flag(void* props, fn_prop_int get, fn_alloc alloc, const char* name, bool& value) {
    char key[96];
    _snprintf_s(key, sizeof(key), _TRUNCATE, "mapflag_%s", name);
    GameStr k;
    if (!make_key(k, key, alloc)) return false;
    g_own_read = true;
    bool ok = false;
    __try { value = get(props, &k, 0) != 0; ok = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    g_own_read = false;
    return ok;
}

} // namespace

uint32_t unlocks_mapflags() {
    constexpr uint32_t kAll = 0xFFFFFFFFu;
    const uintptr_t at = addr_of_data(D_MewDirectorPtr);
    void* dir = nullptr;
    auto get   = (fn_prop_int)addr_of_call(C_PropGetInt);
    auto alloc = (fn_alloc)addr_of_call(C_GameStrAlloc);
    if (!at || !get || !alloc || !mem_read((const void*)at, &dir, sizeof(dir)) || !dir) return kAll;

    uint8_t debug = 0;
    if (mem_read((const uint8_t*)dir + kDir_DebugUnlock, &debug, 1) && debug) return kAll;

    void* props = (uint8_t*)dir + kDir_SaveProps;
    uint32_t flags = 0;
    for (int i = 0; i < kFlagCount; ++i) {
        bool on = false;
        if (!read_flag(props, get, alloc, kFlagNames[i], on)) {
            static bool said = false;
            if (!said) { said = true; log_line_lvl(LogLevel::Warn, "SETUP", "the map unlock flags could not be read -- not restricting the chapter or the map"); }
            return kAll;
        }
        if (on) flags |= 1u << i;
    }
    return flags;
}

bool event_window_on();
bool event_property_answer(const char* key, int64_t local, int64_t& value);

void unlocks_override_set(uint32_t flags) {
    g_override = flags; g_override_on = true; g_override_at = GetTickCount64();
    log_line("SETUP", "the chapter map will be generated from the flags every player has: %08x", (unsigned)flags);
}
void unlocks_override_clear() {
    if (g_override_on) log_line("SETUP", "the chapter map is up -- map flags answer from this player's own save again");
    g_override_on = false;
}
bool unlocks_override_active() {
    if (g_own_read) return false;
    if (g_override_on && GetTickCount64() - g_override_at > kOverrideMaxMs) g_override_on = false;
    return g_override_on || event_window_on();
}
bool unlocks_override_lookup(const char* key, int64_t local, int64_t& value) {
    if (g_override_on && strncmp(key, "mapflag_", 8) == 0) {
        for (int i = 0; i < kFlagCount; ++i)
            if (strcmp(key + 8, kFlagNames[i]) == 0) { value = (g_override >> i) & 1u; return true; }
    }
    return event_property_answer(key, local, value);
}

// ============================================================================================================================================
// the host's unlock lists
// ============================================================================================================================================
namespace {

struct Window {
    bool on = false;
    bool event = false;                // an event node (else a battle node) -- for the log; both kinds last until the map is ready again
    bool left_map = false;             // the map went not-ready, or the battle was built: the next ready map ends the window
    ULONGLONG at = 0;
    unsigned props = 0, props_differing = 0;
    char why[96] = {};
    unsigned checks[5] = {}, overridden[5] = {}, differing[5] = {};
    unsigned logged = 0;
    // What an event property reads as INSIDE this window: the host's value, moved along by what the event itself writes (see event_property_answer).
    bool    prop_have[event_keys::kCount] = {};
    int64_t prop_last[event_keys::kCount] = {};      // this save's own value as of the last read
    int32_t prop_virt[event_keys::kCount] = {};      // what the reads answer
    // the properties of the host's WHOLE table that the 55 above do not name (proto 79): the same rule, found by the key's hash
    static constexpr unsigned kDyn = 512;
    uint32_t dyn_hash[kDyn] = {};
    int64_t  dyn_last[kDyn] = {};
    int32_t  dyn_virt[kDyn] = {};
    unsigned dyn_n = 0, dyn_differing = 0;
} win;

// THE HOST'S WHOLE PROPERTY TABLE (proto 79), as (hash, value): assembled from MSG_PROPS chunks.
struct HostProps {
    bool have = false;
    uint32_t epoch = 0, total = 0, got = 0;
    static constexpr uint32_t kMax = kPropsMax;
    uint32_t hash[kMax] = {};
    int32_t  value[kMax] = {};
    bool lookup(uint32_t h, int32_t& v) const { for (uint32_t i = 0; i < got; ++i) if (hash[i] == h) { v = value[i]; return true; } return false; }
} hprops;

// the key hash both peers use: the low 32 bits of the FNV-1a the game itself hashes a property name with
uint32_t prop_key_hash(const char* key) {
    uint64_t h = 1469598103934665603ULL;
    for (const char* c = key; *c; ++c) { h ^= (uint8_t)*c; h *= 1099511628211ULL; }
    return (uint32_t)(h ^ (h >> 32));
}

// the class-list override of the current window (see unlocks_classes_after)
struct ClassState { uint64_t battle = ~0ull; bool seen_up = false, released = false; uint64_t logged[2] = {}; } cls;

struct HostLists {
    bool have = false;
    uint32_t epoch = 0;
    UnlocksMsg msg;
} host_lists;

constexpr ULONGLONG kWindowMaxMs = 3ull * 60 * 60 * 1000;   // a battle can be long; the ready map ends the window, this only catches a lost one
constexpr unsigned kMaxDifferenceLines = 40;
const char* const kListNames[5] = { "ability", "passive", "item", "level group", "boss" };

int list_size(UnlockList l) {
    switch (l) {
        case UnlockList::Ability:    return unlock_lists::kAbilityCount;
        case UnlockList::Passive:    return unlock_lists::kPassiveCount;
        case UnlockList::Item:       return unlock_lists::kItemCount;
        case UnlockList::LevelGroup: return unlock_lists::kLevelGroupCount;
        case UnlockList::Boss:       return unlock_lists::kBossCount;
    }
    return 0;
}
const char* const* list_names(UnlockList l) {
    switch (l) {
        case UnlockList::Ability:    return unlock_lists::kAbilities;
        case UnlockList::Passive:    return unlock_lists::kPassives;
        case UnlockList::Item:       return unlock_lists::kItems;
        case UnlockList::LevelGroup: return unlock_lists::kLevelGroups;
        case UnlockList::Boss:       return unlock_lists::kBosses;
    }
    return nullptr;
}
int find_name(UnlockList l, const char* name) {
    const char* const* names = list_names(l);
    for (int i = 0; i < list_size(l); ++i) if (strcmp(names[i], name) == 0) return i;
    return -1;
}
bool bit_of(const UnlocksMsg& m, UnlockList l, int i) {
    switch (l) {
        case UnlockList::Ability:    return (m.abilities >> i) & 1u;
        case UnlockList::Passive:    return (m.passives >> i) & 1u;
        case UnlockList::Item:       return (m.items[i >> 3] >> (i & 7)) & 1u;
        case UnlockList::LevelGroup: return (m.levels >> i) & 1u;
        case UnlockList::Boss:       return (m.bosses >> i) & 1u;
    }
    return false;
}
void set_bit(UnlocksMsg& m, UnlockList l, int i) {
    switch (l) {
        case UnlockList::Ability:    m.abilities |= 1u << i; break;
        case UnlockList::Passive:    m.passives |= 1u << i; break;
        case UnlockList::Item:       m.items[i >> 3] |= (uint8_t)(1u << (i & 7)); break;
        case UnlockList::LevelGroup: m.levels |= (uint8_t)(1u << i); break;
        case UnlockList::Boss:       m.bosses |= (uint8_t)(1u << i); break;
    }
}
Target target_of(UnlockList l) {
    switch (l) {
        case UnlockList::Ability:    return T_IsAbilityUnlocked;
        case UnlockList::Passive:    return T_IsPassiveUnlocked;
        case UnlockList::Item:       return T_IsItemUnlocked;
        case UnlockList::LevelGroup: return T_IsLevelUnlocked;
        case UnlockList::Boss:       return T_IsBossAvailable;
    }
    return T_COUNT;
}

using fn_is_unlocked = bool (__fastcall*)(void* save, GameStr* name);
using fn_is_boss = bool (__fastcall*)(void* closure, void* gon);

// This game's own answer for one name -- the same check the detour guards, with the detour told to stand aside.
bool ask(UnlockList l, void* save, fn_is_unlocked fn, fn_alloc alloc, const char* name, bool& value) {
    GameStr k;
    if (!make_key(k, name, alloc)) return false;
    g_own_read = true;
    bool ok = false;
    if (l == UnlockList::Boss) {
        // The boss predicate reads only the std::string at GonObject+0x88 (it copies it and tidies its own copy, and every boss name fits the 15-character
        // buffer), so a stand-in object holding that string answers.
        alignas(16) unsigned char gon[0xC0] = {};
        memcpy(gon + 0x88, &k, sizeof(k));
        __try { value = ((fn_is_boss)fn)(nullptr, gon) != 0; ok = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    } else {
        __try { value = fn(save, &k) != 0; ok = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    }
    g_own_read = false;
    return ok;
}

// This game's own value of one save property (the getter the events' requirements use, with the detour told to stand aside).
bool ask_property(void* props, fn_prop_int get, fn_alloc alloc, const char* name, int64_t& value) {
    GameStr k;
    if (!make_key(k, name, alloc)) return false;
    g_own_read = true;
    bool ok = false;
    __try { value = get(props, &k, 0); ok = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    g_own_read = false;
    return ok;
}

// --- the class list ----------------------------------------------------------------------------------------------------------------------------
// 0x1402322B0(save, std::vector<std::string>* out, bool with_colorless): the names of the classes this SAVE has unlocked (its vector at save+0xC0, "Colorless" kept or dropped by the flag, "Jester"
// special-cased). The pool the jester boss's scramble spell, ConjureBonusAbility & co. draw from is the abilities of those classes, so the LIST decides the pool: its length, and its order (the index
// the draw lands on). The ability checks the mod already answers like the host are asked per ability, AFTER this list has been walked -- they cannot make two different lists the same.
using fn_classes = void* (__fastcall*)(void* save, void* out, bool with_colorless);
struct GameVec { void* b; void* e; void* c; };        // std::vector<std::string>: begin, end, capacity -- the strings are 0x20 bytes each (MSVC: 16-byte buffer, size at +0x10, capacity at +0x18)
// the calls into the game, under SEH, in frames with no destructors
bool call_classes(fn_classes fn, void* save, GameVec* v, bool flag) { __try { fn(save, v, flag); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
void* call_alloc(fn_alloc f, size_t n) { __try { return f(n); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; } }
bool read_class_vector(const GameVec& v, char names[kClassMax][kClassLen], uint8_t& n) {
    n = 0;
    if (!v.b || v.e < v.b) return false;
    const size_t count = ((const uint8_t*)v.e - (const uint8_t*)v.b) / 0x20;
    if (count > kClassMax) return false;
    for (size_t i = 0; i < count; ++i) {
        char t[kClassLen] = {};
        if (!mem_read_std_string((const uint8_t*)v.b + i * 0x20, t, sizeof(t)) || strlen(t) >= kClassLen) return false;
        memcpy(names[i], t, kClassLen);
    }
    n = (uint8_t)count;
    return true;
}
std::string class_text(const char names[kClassMax][kClassLen], uint8_t n) {
    std::string s;
    for (uint8_t i = 0; i < n && i < kClassMax; ++i) { if (i) s += ","; s += names[i]; }
    return s;
}

// --- THE WHOLE PROPERTY TABLE (proto 79) ------------------------------------------------------------------------------------------------------
// WHERE IT LIVES (2026-10-04, two live tests): the std::unordered_map at the start of the properties object that the getter (0x14022C5E0) looks in FIRST is only a CACHE of the keys read so far
// -- its element count was 0 at the host's publish, a layout that read fine and held nothing. The keys themselves are in the `properties` table of the save file, so the whole table is read
// from there (read-only, the save as the game last wrote it) and every integer row is sent. Rows whose data is not a whole number (reals, text) are left out.
struct PropPair { uint32_t hash; int32_t value; };
bool whole_int_text(const std::string& t, int64_t& v) {
    if (t.empty() || t.size() > 20) return false;
    char* end = nullptr;
    const long long x = strtoll(t.c_str(), &end, 10);
    if (end != t.c_str() + t.size()) return false;
    v = x;
    return true;
}
// the integer rows of the host's save file; 0 ok, 1 no path, 2 the file could not be read, 3 no row
int read_props_from_file(PropPair* out, uint32_t cap, uint32_t& n, std::string& sample) {
    n = 0;
    wchar_t path[MAX_PATH] = {};
    if (!savefile_live_path(path, MAX_PATH)) return 1;
    std::vector<std::pair<std::string, std::string>> rows;
    if (!checkpoint_io::read_all_properties(path, rows)) return 2;
    for (const auto& r : rows) {
        int64_t v = 0;
        if (!whole_int_text(r.second, v) || n >= cap) continue;
        out[n].hash = prop_key_hash(r.first.c_str());
        out[n].value = v < -2000000000LL ? -2000000000 : v > 2000000000LL ? 2000000000 : (int32_t)v;
        ++n;
        if (sample.size() < 80) { if (!sample.empty()) sample += ","; sample += r.first; }
    }
    return n ? 0 : 3;
}

void props_publish() {
    static PropPair pairs[kPropsMax];
    static bool said_bad = false;
    const uintptr_t at = addr_of_data(D_MewDirectorPtr);
    void* dir = nullptr;
    auto alloc = (fn_alloc)addr_of_call(C_GameStrAlloc);
    auto get = (fn_prop_int)addr_of_call(C_PropGetInt);
    if (!at || !alloc || !get || !mem_read((const void*)at, &dir, sizeof(dir)) || !dir) return;
    void* save = (uint8_t*)dir + kDir_SaveProps;
    uint32_t n = 0;
    std::string sample;
    const int why = read_props_from_file(pairs, kPropsMax, n, sample);
    if (why) {
        if (!said_bad) {
            said_bad = true;
            log_line_lvl(LogLevel::Warn, "UNLOCK", "!! the save's property table could not be read from the save file (%s) -- only the known keys are sent",
                         why == 1 ? "no save path yet" : why == 2 ? "the file could not be opened or queried" : "no integer row");
        }
        return;
    }
    unsigned found = 0, equal = 0;
    for (int i = 0; i < event_keys::kCount; ++i) {
        int64_t v = 0;
        if (!ask_property(save, get, alloc, event_keys::kKeys[i], v)) continue;
        const uint32_t h = prop_key_hash(event_keys::kKeys[i]);
        for (uint32_t k = 0; k < n; ++k) if (pairs[k].hash == h) { ++found; if ((int64_t)pairs[k].value == (v < -2000000000LL ? -2000000000 : v > 2000000000LL ? 2000000000 : v)) ++equal; break; }
    }
    // the save file is what the game last WROTE: a counter changed since may differ, so most (not all) of the known keys have to agree with the live getter
    if (found < 8 || equal * 10 < found * 7) {
        if (!said_bad) { said_bad = true; log_line_lvl(LogLevel::Warn, "UNLOCK", "!! the property table read does not agree with the getter (%u of %u known keys equal; first rows: %s) -- it is NOT sent", equal, found, sample.c_str()); }
        return;
    }
    PropsMsg m;
    m.epoch = (uint32_t)GetTickCount() | 1u; m.total = n;
    bool ok = true;
    for (uint32_t first = 0; first < n && ok; first += kPropsChunk) {
        m.first = first; m.count = (uint8_t)((n - first) < kPropsChunk ? (n - first) : kPropsChunk);
        for (uint32_t i = 0; i < m.count; ++i) { m.hash[i] = pairs[first + i].hash; m.value[i] = pairs[first + i].value; }
        ok = net_send_props(m);
    }
    static uint32_t last_n = ~0u;
    if (!ok) log_line_lvl(LogLevel::Warn, "UNLOCK", "the host's property table could not be sent");
    else if (n != last_n) { last_n = n; log_line("UNLOCK", "the host's whole save-property table sent to the clients: %u integer properties from the save file (%u of %u known keys equal to the live getter)", n, equal, found); }
}

bool snapshot(UnlocksMsg& out) {
    const uintptr_t at = addr_of_data(D_MewDirectorPtr);
    void* dir = nullptr;
    auto alloc = (fn_alloc)addr_of_call(C_GameStrAlloc);
    if (!at || !alloc || !mem_read((const void*)at, &dir, sizeof(dir)) || !dir) return false;
    void* save = (uint8_t*)dir + kDir_SaveProps;
    out = UnlocksMsg{};
    out.epoch = (uint32_t)GetTickCount() | 1u;
    out.n_abilities = (uint8_t)unlock_lists::kAbilityCount;
    out.n_passives = (uint8_t)unlock_lists::kPassiveCount;
    out.n_items = (uint8_t)unlock_lists::kItemCount;
    out.n_levels = (uint8_t)unlock_lists::kLevelGroupCount;
    out.n_bosses = (uint8_t)unlock_lists::kBossCount;
    for (int li = 0; li < 5; ++li) {
        const UnlockList l = (UnlockList)li;
        auto fn = (fn_is_unlocked)addr_of(target_of(l));
        if (!fn) return false;
        for (int i = 0; i < list_size(l); ++i) {
            bool v = false;
            if (!ask(l, save, fn, alloc, list_names(l)[i], v)) return false;
            if (v) set_bit(out, l, i);
        }
    }
    auto get = (fn_prop_int)addr_of_call(C_PropGetInt);
    if (!get) return false;
    out.n_events = (uint8_t)event_keys::kCount;
    for (int i = 0; i < event_keys::kCount; ++i) {
        int64_t v = 0;
        if (!ask_property(save, get, alloc, event_keys::kKeys[i], v)) return false;
        out.events[i] = v < -2000000000LL ? -2000000000 : v > 2000000000LL ? 2000000000 : (int32_t)v;
    }
    // proto 78: the class names the game's class-list function returns for this save, for both flag values (what the ability pools of "any unlocked class" are built from)
    if (auto fn = (fn_classes)addr_of(T_UnlockedClasses)) {
        for (int f = 0; f < 2; ++f) {
            GameVec v{};
            if (!call_classes(fn, save, &v, f != 0)) continue;                             // the returned vector's buffer is the game's; it is not freed here (a few hundred bytes per publish)
            uint8_t n = 0;
            if (read_class_vector(v, out.classes[f], n)) out.n_classes[f] = n;
        }
        log_line("UNLOCK", "the host's class list: without Colorless [%s], with [%s]", out.n_classes[0] == 255 ? "not read" : class_text(out.classes[0], out.n_classes[0]).c_str(),
                 out.n_classes[1] == 255 ? "not read" : class_text(out.classes[1], out.n_classes[1]).c_str());
    }
    return true;
}

} // namespace

// ============================================================================================================================================
// the party's spawn order
// ============================================================================================================================================
// sub_14035C3E0 places the party's cats in an order it sorts first: it lists the party's CatData* in party order (sub_1403B2F30) and
// insertion-sorts them (sub_140364760, its only caller) by each cat's computed stats (sub_1400C1820, +0x10 of the block: SPEED), highest
// first, ties keeping the party order. Measured 2026-10-02: three cats with byte-identical images on both peers sorted 2,3,1 on the host and
// 2,1,3 on the client; the spawn then drew from the random stream in another order, and every cat, pickup and spiderling placed after them
// differed (36 vs 38 units, halt at turn 0). So the host sends the order its own sort will produce (with the unlock answers, before the node
// is entered), a client whose sort came out differently is put into it, and both peers log each party cat's stats and their inputs -- the
// cause of two machines disagreeing on one cat's speed is still open.
namespace {

using fn_cat_stats = int* (__fastcall*)(void* cat, int* out, const void* filter, bool full, bool no_kitten_penalty);
using fn_cat_by_id = void* (__fastcall*)(void* registry, uint64_t id);
using fn_is_kitten = bool (__fastcall*)(void* cat);
constexpr int kSortStat = 4;                     // STR DEX CON INT SPD CHA LCK: the sort compares SPEED
constexpr uint32_t kMaxSpawn = 8;

struct Predicted { uint32_t n = 0; uint64_t ids[kMaxSpawn] = {}; int32_t keys[kMaxSpawn] = {}; } predicted;   // the order the host last sent

// The game's own stats of one cat, asked exactly as the sort asks (filter all -1, full, the kitten byte 0).
bool cat_stats(void* cat, int out[7]) {
    auto fn = (fn_cat_stats)addr_of_call(C_CatStats);
    if (!fn || !cat) return false;
    alignas(16) uint8_t filter[32];
    memset(filter, 0xFF, sizeof(filter));
    alignas(16) int buf[16] = {};
    bool ok = false;
    __try {
        const int* r = fn(cat, buf, filter, true, false);
        if (r) { memcpy(out, r, 7 * sizeof(int)); ok = true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

uint64_t cat_id(void* cat) {
    uint64_t id = 0;
    return cat && mem_read((const uint8_t*)cat + kCatData_SaveId, &id, sizeof(id)) ? id : 0;
}

int kitten_answer(void* cat) {
    auto fn = (fn_is_kitten)addr_of(T_IsKitten);          // through the hook: the answer the game gets
    if (!fn) return -1;
    int v = -1;
    __try { v = fn(cat) ? 1 : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { v = -1; }
    return v;
}

void by_id_safe(fn_cat_by_id by_id, void* registry, uint64_t id, void** out) {
    __try { *out = by_id(registry, id); } __except (EXCEPTION_EXECUTE_HANDLER) { *out = nullptr; }
}

// The run's party (not the familiars) in the party list's order, with each cat's CatData* -- what sub_1403B2F30 hands the sort.
uint32_t party_cats(uint64_t* ids, void** cats, uint32_t max) {
    const uintptr_t at = addr_of_data(D_MewDirectorPtr);
    auto by_id = (fn_cat_by_id)addr_of_call(C_CatDataById);
    const uint8_t* dir = nullptr;
    void* registry = nullptr;
    uint32_t count = 0;
    const uint64_t* data = nullptr;
    if (!at || !by_id || !mem_read((const void*)at, &dir, sizeof(dir)) || !dir ||
        !mem_read(dir + kDir_CatRegistry, &registry, sizeof(registry)) || !registry ||
        !mem_read(dir + kDir_CatIdCount, &count, sizeof(count)) || !mem_read(dir + kDir_CatIdData, &data, sizeof(data)) ||
        !data || !count || count > max || !mem_read(data, ids, count * sizeof(uint64_t))) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        by_id_safe(by_id, registry, ids[i], &cats[i]);
        if (!cats[i]) return 0;
    }
    return count;
}

uint64_t stream_s0() {
    uint64_t s0 = 0;
    if (const uint64_t* s = rng_global_stream()) mem_read(s, &s0, sizeof(s0));
    return s0;
}

// The order every peer places the party in: the game's own key (SPEED, highest first) with the PARTY LIST breaking ties. The game breaks them
// with a shuffle of the shared stream (0x140086F00, just before its sort) -- measured 2026-10-02, the host's shuffle gave 2,3,1 and the client's
// 2,1,3 from the same list -- so the tie-break is taken out of the stream's hands. The shuffle's draws are still made, the same number on both.
bool predict_spawn(UnlocksMsg& m) {
    uint64_t ids[kMaxSpawn] = {};
    void* cats[kMaxSpawn] = {};
    const uint32_t n = party_cats(ids, cats, kMaxSpawn);
    if (!n) return false;
    int key[kMaxSpawn] = {};
    for (uint32_t i = 0; i < n; ++i) {
        int s[7] = {};
        if (!cat_stats(cats[i], s)) return false;
        key[i] = s[kSortStat];
    }
    uint32_t order[kMaxSpawn] = {};
    for (uint32_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order, order + n, [&](uint32_t a, uint32_t b) { return key[a] > key[b]; });
    m.n_spawn = (uint8_t)n;
    for (uint32_t i = 0; i < n; ++i) { m.spawn_ids[i] = ids[order[i]]; m.spawn_keys[i] = key[order[i]]; }
    return true;
}

void append(char* line, size_t cap, int& off, const char* fmt, uint64_t id, int key) {
    if (off < (int)cap - 32) off += _snprintf_s(line + off, cap - off, _TRUNCATE, fmt, (unsigned long long)id, key);
}

// One party cat, as ONE line of text: the stats the sort reads and every input the stats come from (sub_1400C0B80 / sub_1400C1820 read them all off the cat).
// The fingerprint is a hash of that text -- two peers' fingerprints of the same cat agree exactly when every printed field does.
void describe_cat_inputs(void* cat, char* out, size_t cap, uint64_t& fp) {
    int s[7] = {};
    const bool have = cat_stats(cat, s);
    uint64_t flags = 0;
    mem_read((const uint8_t*)cat + kCatData_Flags, &flags, sizeof(flags));
    int a[7] = {}, b[7] = {}, c[7] = {};
    mem_read((const uint8_t*)cat + 0x6F0, a, sizeof(a));
    mem_read((const uint8_t*)cat + 0x70C, b, sizeof(b));
    mem_read((const uint8_t*)cat + 0x728, c, sizeof(c));
    char cls[48] = "?";
    mem_read_std_string((const uint8_t*)cat + 0xC10, cls, sizeof(cls));
    char list[240] = {};
    int off = 0;
    for (unsigned k = 0; k < 5; ++k) {
        char name[40] = "-";
        uint64_t len = 0;
        if (mem_read((const uint8_t*)cat + 0x910 + k * 0x20 + 0x10, &len, sizeof(len)) && len && len < 64)
            mem_read_std_string((const uint8_t*)cat + 0x910 + k * 0x20, name, sizeof(name));
        off += _snprintf_s(list + off, sizeof(list) - off, _TRUNCATE, "%s%s", k ? "," : "", name);
    }
    char gear[240] = {};
    off = 0;
    for (unsigned k = 0; k < 5; ++k) {
        const uint8_t* slot = (const uint8_t*)cat + 0x9B0 + k * 0x60;
        char name[40] = "-";
        uint64_t len = 0;
        if (mem_read(slot + 8 + 0x10, &len, sizeof(len)) && len && len < 64) mem_read_std_string(slot + 8, name, sizeof(name));
        off += _snprintf_s(gear + off, sizeof(gear) - off, _TRUNCATE, "%s%s", k ? "," : "", name);
    }
    // GEAR AND ABILITIES BEFORE THE LONG ARRAYS (2026-10-05): the audit's text travels in a fixed-size field (about 270 characters) and was cut inside the gear list, so "this cat's gear differs" -- the one
    // thing a pre-battle difference usually is -- could not be read in the log. The fingerprint is taken over the whole text, the copy that travels is cut.
    char full[900] = {};
    _snprintf_s(full, sizeof(full), _TRUNCATE, "%s STR %d DEX %d CON %d INT %d SPD %d CHA %d LCK %d | image %016llx | flags %016llx | kitten %d | class '%s' |"
                      " gear [%s] | +910 [%s] | +6F0 [%d %d %d %d %d %d %d] +70C [%d %d %d %d %d %d %d] +728 [%d %d %d %d %d %d %d]",
             have ? "stats" : "STATS UNREADABLE", s[0], s[1], s[2], s[3], s[4], s[5], s[6],
             (unsigned long long)catsync_image_hash(cat), (unsigned long long)flags, kitten_answer(cat), cls, gear, list,
             a[0], a[1], a[2], a[3], a[4], a[5], a[6], b[0], b[1], b[2], b[3], b[4], b[5], b[6], c[0], c[1], c[2], c[3], c[4], c[5], c[6]);
    strncpy_s(out, cap, full, _TRUNCATE);
    fp = 1469598103934665603ULL;                         // FNV-1a, the same as mgmp_lockstep's: only this peer's text against the other's
    for (const char* c = full; *c; ++c) { fp ^= (uint8_t)*c; fp *= 1099511628211ULL; }
}

void log_cat_inputs(void* cat) {
    char text[900] = {};
    uint64_t fp = 0;
    describe_cat_inputs(cat, text, sizeof(text), fp);
    log_line("SPAWN", "  cat %016llx: %s", (unsigned long long)cat_id(cat), text);
}

} // namespace

namespace {
uint64_t splitmix64(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
} // namespace

// Measured 2026-10-02 (RNGTRACE): the stream agreed at the node entry (NODEHASH) and already differed at the first definition the battle loaded,
// on every test the same way -- so the board (statics, the party's shuffle, pickups, spiderlings) was placed from two different streams. Every
// peer now starts the build from a stream derived from the battle id (the node seed both share).
namespace { bool g_in_build = false; uint32_t g_load_no = 0; }

// ---- the director's per-run queues the battle build consumes (see the protocol: PendingEnemy) ----------------------------------------------------------
//
// Each peer keeps them in its own save. A: enemies that come back (spawned by the enemy stage, `remaining` counts down), B: neutral critters (frogs;
// spawned at the end of the build, `remaining` counts down), C: units carried along (EVERY entry spawns in the enemy stage; no counter).
namespace {
struct QueueDesc {
    const char* label;
    uintptr_t   begin, end, stride, name;
    int         count, hp, mode;     // field offsets, -1 = none
};
constexpr uint32_t kTableQueues = 3;   // the director vectors; queue 3 is the modifier list, handled apart
constexpr QueueDesc kQueues[kTableQueues] = {
    { "returning enemies", 0x610, 0x618, 0x38, 0x00, 0x24, 0x20, 0x28 },
    { "battle-start spawns", 0x650, 0x658, 0xB8, 0x88, 0xB0, -1,   -1   },
    { "carried units",     0x628, 0x630, 0x58, 0x00, -1,   0x20, 0x24 },
};
struct PendingLocal { uintptr_t at = 0; char name[24] = {}; PendingEnemy e; };
uint32_t pending_ident(const char* name) {
    uint64_t h = 1469598103934665603ull;
    for (const char* c = name; *c; ++c) { h ^= (uint8_t)*c; h *= 1099511628211ull; }
    return (uint32_t)(h ^ (h >> 32)) | 1u;
}
const uint8_t* pending_director() {
    const uintptr_t at = addr_of_data(D_MewDirectorPtr);
    const uint8_t* dir = nullptr;
    return (at && mem_read((const void*)at, &dir, sizeof(dir)) && dir) ? dir : nullptr;
}
// Every entry of one queue (also the used-up ones: their addresses are what an alignment writes to). A queue without a counter is "always live" (remaining 1).
uint32_t pending_read(const QueueDesc& q, PendingLocal* out, uint32_t cap) {
    const uint8_t* dir = pending_director();
    if (!dir) return 0;
    uintptr_t b = 0, e = 0;
    if (!mem_read(dir + q.begin, &b, sizeof(b)) || !mem_read(dir + q.end, &e, sizeof(e)) || !b || e < b) return 0;
    uint32_t n = (uint32_t)((e - b) / q.stride);
    if (n > cap) n = cap;
    uint32_t got = 0;
    for (uint32_t i = 0; i < n; ++i) {
        PendingLocal& p = out[got];
        p = PendingLocal{};
        p.at = b + (uintptr_t)i * q.stride;
        if (!mem_read_std_string((const void*)(p.at + q.name), p.name, sizeof(p.name))) continue;
        p.e.remaining = 1;
        if (q.count >= 0 && !mem_read((const void*)(p.at + q.count), &p.e.remaining, 4)) continue;
        if (q.hp >= 0)   mem_read((const void*)(p.at + q.hp), &p.e.hp, 4);
        if (q.mode >= 0) mem_read((const void*)(p.at + q.mode), &p.e.mode, 4);
        p.e.ident = pending_ident(p.name);
        ++got;
    }
    return got;
}
void pending_log(const char* who, const QueueDesc& q, const PendingLocal* l, uint32_t n) {
    char line[320] = {};
    int off = 0;
    for (uint32_t i = 0; i < n && off < (int)sizeof(line) - 60; ++i)
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " %s x%d (hp %d, %d)%s", l[i].name, l[i].e.remaining, l[i].e.hp, l[i].e.mode, l[i].e.remaining > 0 ? "" : " [used up]");
    log_line("SPAWN", "%s: the director's queue of %s holds %u entr%s:%s", who, q.label, n, n == 1 ? "y" : "ies", n ? line : " none");
}
// Queue C is hidden by moving its end pointer back; this remembers how to put it back.
struct HiddenQueue { bool on = false; uintptr_t end_at = 0, begin = 0, end = 0; } g_hidden;
} // namespace



// ---- the director's weather list (vector<std::string>, +0x668 / +0x670 / +0x678, 32-byte MSVC strings) ----------------------------------------------------
namespace {
constexpr uintptr_t kDir_Weather = 0x668;
constexpr uint32_t  kStrSize = 0x20;
struct WeatherNames { uint8_t n = 0; char name[kWeatherNames][kWeatherLen] = {}; };
bool weather_read(WeatherNames& out) {
    out = WeatherNames{};
    const uint8_t* dir = pending_director();
    if (!dir) return false;
    uintptr_t b = 0, e = 0;
    if (!mem_read(dir + kDir_Weather, &b, sizeof(b)) || !mem_read(dir + kDir_Weather + 8, &e, sizeof(e)) || e < b) return false;
    if (!b) return true;                                    // an empty vector: no weather
    const uintptr_t count = (e - b) / kStrSize;
    for (uintptr_t i = 0; i < count && out.n < kWeatherNames; ++i)
        if (mem_read_std_string((const void*)(b + i * kStrSize), out.name[out.n], kWeatherLen)) ++out.n;
    return true;
}
void weather_log(const char* who, const WeatherNames& w) {
    char line[200] = {};
    int off = 0;
    for (uint32_t i = 0; i < w.n && off < (int)sizeof(line) - 40; ++i) off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " %s", w.name[i]);
    log_line("SPAWN", "%s: the director's weather list holds %u:%s", who, (unsigned)w.n, w.n ? line : " none");
}
// A private copy of the host's names, laid out as MSVC std::strings (16-byte inline buffer / pointer, size at +0x10, capacity at +0x18); a name longer than 15
// characters keeps its characters in the pool next to it, with capacity = length, as a heap string would. Nothing here is ever freed by the game: the vector
// is pointed back at the client's own storage before anything could grow or destroy it.
struct WeatherSwap {
    bool on = false;
    uintptr_t at = 0, orig_first = 0, orig_last = 0, orig_end = 0, ours = 0;
    alignas(16) uint8_t strings[kWeatherNames * kStrSize] = {};
    char pool[kWeatherNames][kWeatherLen] = {};
    bool left_map = false;
    ULONGLONG at_ms = 0;
} g_wswap;
} // namespace

static void weather_restore() {
    if (!g_wswap.on) return;
    g_wswap.on = false;
    uintptr_t f = 0, l = 0;
    if (!mem_read((const void*)g_wswap.at, &f, sizeof(f)) || !mem_read((const void*)(g_wswap.at + 8), &l, sizeof(l)) || f != g_wswap.ours) {
        log_line_lvl(LogLevel::Warn, "SPAWN", "!! the director's weather list changed while the host's weather stood in for it -- it is left as the game has it");
        return;
    }
    mem_write((void*)g_wswap.at, &g_wswap.orig_first, sizeof(uintptr_t));
    mem_write((void*)(g_wswap.at + 8), &g_wswap.orig_last, sizeof(uintptr_t));
    mem_write((void*)(g_wswap.at + 16), &g_wswap.orig_end, sizeof(uintptr_t));
    log_line("SPAWN", "this peer's own weather list put back (the map is ready again)");
}

// The host's names written into the client's OWN vector<std::string> storage (`at` = the vector, three pointers): an existing string is overwritten in its own buffer when the new
// name fits its capacity, the spare capacity behind the end pointer takes fresh inline strings for more names, and the end pointer moves. False (nothing written) when it does not fit.
static bool weather_write_in_place(uintptr_t at, const WeatherNames& host) {
    uintptr_t first = 0, last = 0, endcap = 0;
    if (!mem_read((const void*)at, &first, 8) || !mem_read((const void*)(at + 8), &last, 8) || !mem_read((const void*)(at + 16), &endcap, 8) || !first || last < first || endcap < last) return false;
    const uintptr_t have = (last - first) / kStrSize, room = (endcap - first) / kStrSize;
    if (host.n > room) return false;
    // check everything fits BEFORE writing anything
    for (uint32_t i = 0; i < host.n; ++i) {
        const size_t len = strnlen(host.name[i], kWeatherLen - 1);
        if (i < have) {
            uint64_t cap = 0;
            if (!mem_read((const void*)(first + i * kStrSize + 0x18), &cap, 8) || len > cap) return false;
        } else if (len > 15) return false;
    }
    for (uint32_t i = 0; i < host.n; ++i) {
        const uintptr_t s = first + i * kStrSize;
        const size_t len = strnlen(host.name[i], kWeatherLen - 1);
        uint64_t size = len, cap = 15;
        if (i < have) {
            if (!mem_read((const void*)(s + 0x18), &cap, 8)) return false;
            uintptr_t buf = s;                                   // inline buffer
            if (cap > 15 && !mem_read((const void*)s, &buf, 8)) return false;   // heap buffer
            if (!mem_write((void*)buf, host.name[i], len + 1)) return false;
        } else {
            uint8_t fresh[kStrSize] = {};
            memcpy(fresh, host.name[i], len + 1);
            memcpy(fresh + 0x10, &size, 8);
            memcpy(fresh + 0x18, &cap, 8);
            if (!mem_write((void*)s, fresh, kStrSize)) return false;
            continue;
        }
        if (!mem_write((void*)(s + 0x10), &size, 8)) return false;
    }
    const uintptr_t newlast = first + (uintptr_t)host.n * kStrSize;
    return mem_write((void*)(at + 8), &newlast, 8);
}

// CLIENT, start of the build: the host's weather list replaces this peer's own for the battle when they differ (in order: the build walks the list in order).
static void weather_align(const WeatherNames& host) {
    weather_restore();                                       // (a battle that never gave the map back)
    WeatherNames mine;
    if (!weather_read(mine)) return;
    bool same = mine.n == host.n;
    for (uint32_t i = 0; same && i < host.n; ++i) same = strcmp(mine.name[i], host.name[i]) == 0;
    if (same) { log_line("SPAWN", "weather list equals the host's (%u)", (unsigned)host.n); return; }
    const uint8_t* dir = pending_director();
    if (!dir) return;
    if (weather_write_in_place((uintptr_t)dir + kDir_Weather, host)) {
        weather_log("client, this peer's own weather (replaced for good)", mine);
        log_line_lvl(LogLevel::Warn, "SPAWN", "!! this peer's weather list differed from the host's -- it now holds the host's %u weather(s) and stays that way", (unsigned)host.n);
        return;
    }
    g_wswap = WeatherSwap{};
    g_wswap.at = (uintptr_t)dir + kDir_Weather;
    if (!mem_read((const void*)g_wswap.at, &g_wswap.orig_first, 8) || !mem_read((const void*)(g_wswap.at + 8), &g_wswap.orig_last, 8) || !mem_read((const void*)(g_wswap.at + 16), &g_wswap.orig_end, 8)) return;
    for (uint32_t i = 0; i < host.n; ++i) {
        uint8_t* s = g_wswap.strings + i * kStrSize;
        const size_t len = strnlen(host.name[i], kWeatherLen - 1);
        uint64_t size = len, cap = 15;
        if (len <= 15) memcpy(s, host.name[i], len + 1);
        else { memcpy(g_wswap.pool[i], host.name[i], len + 1); const uint64_t p = (uint64_t)(uintptr_t)g_wswap.pool[i]; memcpy(s, &p, 8); cap = len; }
        memcpy(s + 0x10, &size, 8);
        memcpy(s + 0x18, &cap, 8);
    }
    g_wswap.ours = (uintptr_t)g_wswap.strings;
    const uintptr_t last = g_wswap.ours + (uintptr_t)host.n * kStrSize;
    mem_write((void*)g_wswap.at, &g_wswap.ours, 8);
    mem_write((void*)(g_wswap.at + 8), &last, 8);
    mem_write((void*)(g_wswap.at + 16), &last, 8);
    g_wswap.on = true;
    g_wswap.at_ms = GetTickCount64();
    weather_log("client, this peer's own weather (set aside for this battle)", mine);
    log_line_lvl(LogLevel::Warn, "SPAWN", "!! this peer's weather list differs from the host's -- the host's %u weather(s) are used for this battle", (unsigned)host.n);
}

// HOST: its queues, for the message that carries the level (live entries only).
static void pending_fill(UnlocksMsg& m) {
    {   // the weather list
        WeatherNames w;
        weather_read(w);
        m.n_weather = w.n;
        memcpy(m.weather, w.name, sizeof(m.weather));
    }
    for (uint32_t q = 0; q < kTableQueues; ++q) {
        PendingLocal l[16];
        const uint32_t n = pending_read(kQueues[q], l, 16);
        m.n_pending[q] = 0;
        for (uint32_t i = 0; i < n && m.n_pending[q] < kPendingMax; ++i) if (l[i].e.remaining > 0) m.pending[q][m.n_pending[q]++] = l[i].e;
    }
}

static void pending_unhide() {
    if (!g_hidden.on) return;
    g_hidden.on = false;
    const uint8_t* dir = pending_director();
    uintptr_t b = 0, e = 0;
    if (!dir || !mem_read(dir + kQueues[2].begin, &b, sizeof(b)) || !mem_read(dir + kQueues[2].end, &e, sizeof(e)) || b != g_hidden.begin || e != g_hidden.begin) {
        log_line_lvl(LogLevel::Warn, "SPAWN", "!! the hidden queue of carried units changed during the build -- it is left as the game has it");
        return;
    }
    mem_write((void*)g_hidden.end_at, &g_hidden.end, sizeof(g_hidden.end));
}


// HOST, start of EVERY battle build: the weather list and the queues the build consumes, tagged with the battle id. The full unlock snapshot rides along (the clients keep it as
// their host_lists as with any publication); no level and no spawn order -- those have their own messages.
static void publish_build_info() {
    if (!net_active() || net_role() != NetRole::Host) return;
    const uint64_t id = lockstep_battle_id();
    if (!id) return;
    UnlocksMsg m;
    if (!snapshot(m)) return;
    // The spawn order rides along: a client keeps only the LAST unlock message as its host_lists, and the spawn sort (later in this build) reads the order from it -- a message
    // without it replaced the one that had it, and every battle ended in "the host's spawn keys have not arrived" and a sync-trouble notice (test 01:02).
    if (!predict_spawn(m)) m.n_spawn = 0;
    pending_fill(m);
    m.build_info = 1;
    m.level_node = id;
    if (!net_send_unlocks(m)) return;
    log_line("SPAWN", "the host's build information (weather list, queues) sent to the clients for battle %016llx", (unsigned long long)id);
}

void unlocks_event_roll() {
    if (!net_active() || net_peer_count() < 2) return;
    uint64_t* s = rng_global_stream();
    const uint64_t id = lockstep_battle_id();
    if (!s || !id) return;
    static uint64_t node = 0;
    static uint32_t n = 0;
    if (node != id) { node = id; n = 0; }
    uint64_t before = 0;
    mem_read(s, &before, sizeof(before));
    uint64_t x = id ^ 0x6D676D7065766E74ull ^ ((uint64_t)(++n) * 0x9E3779B97F4A7C15ull);
    uint64_t st[4];
    for (int i = 0; i < 4; ++i) st[i] = splitmix64(x);
    mem_write(s, st, sizeof(st));
    log_line("EVENT", "outcome pick %u on node %016llx: the shared stream was %016llx, set to %016llx (the same on every peer)", n, (unsigned long long)id, (unsigned long long)before, (unsigned long long)st[0]);
}

bool unlocks_event_subject(void* vec, void* stream, void* pick, void** out) {
    if (!net_active() || net_peer_count() < 2 || !vec || !pick || !out) return false;
    uint32_t n = 0;
    void** data = nullptr;
    if (!mem_read((uint8_t*)vec + 4, &n, sizeof(n)) || !n || n > 32 || !mem_read((uint8_t*)vec + 8, &data, sizeof(data)) || !data) return false;
    void* cats[32] = {};
    if (!mem_read(data, cats, n * sizeof(void*))) return false;
    uint64_t ids[32] = {};
    uint32_t order[32] = {};
    for (uint32_t i = 0; i < n; ++i) { ids[i] = cat_id(cats[i]); if (!ids[i]) return false; order[i] = i; }
    std::sort(order, order + n, [&](uint32_t a, uint32_t b) { return ids[a] < ids[b]; });
    // the draw: the game's own helper over a stand-in vector of the indices 0..n-1 (it reads the count at +4 and the elements at +8 and returns one of them)
    void* idx[32] = {};
    for (uint32_t i = 0; i < n; ++i) idx[i] = (void*)(uintptr_t)i;
    uint8_t fake[24] = {};
    void* fp = idx;
    memcpy(fake + 4, &n, sizeof(n));
    memcpy(fake + 8, &fp, sizeof(fp));
    const uintptr_t k = (uintptr_t)((void*(__fastcall*)(void*, void*))pick)(fake, stream);
    if (k >= n) return false;
    *out = cats[order[k]];
    char line[400] = {};
    int off = 0;
    for (uint32_t i = 0; i < n; ++i) append(line, sizeof(line), off, " %llx", ids[order[i]], 0);
    log_line("EVENT", "the event's subject cat: %u candidate(s) sorted by id [%s ] -- draw %u -> cat %llx (the same on every peer)", n, line, (unsigned)k, (unsigned long long)ids[order[k]]);
    return true;
}

void unlocks_build_enemies_begin() {
    if (!g_in_build || !net_active() || net_peer_count() < 2) return;
    const bool client = net_role() == NetRole::Client;
    PendingLocal l[kTableQueues][16];
    uint32_t n[kTableQueues];
    for (uint32_t q = 0; q < kTableQueues; ++q) {
        n[q] = pending_read(kQueues[q], l[q], 16);
        pending_log(client ? "client, enemy stage" : "host, enemy stage", kQueues[q], l[q], n[q]);
    }
    {
        WeatherNames w;
        weather_read(w);
        weather_log(client ? "client, build start" : "host, build start", w);
    }
    if (!client) {   // the host: everything a client's build must match, for THIS battle -- whatever kind of node it is (a mini-boss or boss never picks its level)
        publish_build_info();
        return;
    }
    {   // the weather first: the build walks the list before anything else. The host's build information may still be on its way: wait for it (it is sent at the START of the host's
        // build, which normally comes first; the receive thread fills a mailbox).
        char hw[kWeatherNames][kWeatherLen] = {};
        uint8_t hwn = 0;
        PendingEnemy hp[kPendingQueues][kPendingMax];
        uint8_t hpn[kPendingQueues] = {};
        const ULONGLONG t0 = GetTickCount64();
        while (!net_host_pending(lockstep_battle_id(), hp, hpn) && GetTickCount64() - t0 < tune::kLevelWaitMs) Sleep(5);
        if (net_host_weather(lockstep_battle_id(), hw, hwn)) {
            WeatherNames host;
            host.n = hwn;
            memcpy(host.name, hw, sizeof(host.name));
            weather_align(host);
        } else {
            log_line_lvl(LogLevel::Warn, "SPAWN", "!! the host's build information for battle %016llx did not arrive within %u ms -- this peer builds with its own", (unsigned long long)lockstep_battle_id(), (unsigned)tune::kLevelWaitMs);
            room_sync_trouble("the host's weather and queues");
        }
    }
    PendingEnemy host[kPendingQueues][kPendingMax];
    uint8_t hn[kPendingQueues] = {};
    const uint64_t id = lockstep_battle_id();
    if (!net_host_pending(id, host, hn)) {
        log_line_lvl(LogLevel::Warn, "SPAWN", "!! the host's queues did not come with its level for battle %016llx -- this peer spawns its own", (unsigned long long)id);
        return;
    }
    for (uint32_t q = 0; q < kTableQueues; ++q) {
        const QueueDesc& qd = kQueues[q];
        bool used[kPendingMax] = {};
        uint32_t off_n = 0, set_n = 0, unmatched = 0;
        for (uint32_t i = 0; i < n[q]; ++i) {
            if (l[q][i].e.remaining <= 0) continue;
            int match = -1;
            for (uint32_t k = 0; k < hn[q] && match < 0; ++k) if (!used[k] && host[q][k].ident == l[q][i].e.ident) match = (int)k;
            if (match < 0) {   // the host has no such entry: this one must not spawn
                ++unmatched;
                if (qd.count >= 0) {
                    const int32_t zero = 0;
                    mem_write((void*)(l[q][i].at + qd.count), &zero, 4);
                    ++off_n;
                    log_line_lvl(LogLevel::Warn, "SPAWN", "!! '%s' (%s) is queued on this peer only -- switched off for this build (the host's battle does not have it)", l[q][i].name, qd.label);
                }
                continue;
            }
            used[match] = true;
            const PendingEnemy& h = host[q][match];
            if (qd.count >= 0 && h.remaining != l[q][i].e.remaining) { mem_write((void*)(l[q][i].at + qd.count), &h.remaining, 4); ++set_n; }
            if (qd.hp >= 0 && h.hp != l[q][i].e.hp)                  { mem_write((void*)(l[q][i].at + qd.hp), &h.hp, 4); ++set_n; }
            if (qd.mode >= 0 && h.mode != l[q][i].e.mode)            { mem_write((void*)(l[q][i].at + qd.mode), &h.mode, 4); ++set_n; }
        }
        if (qd.count < 0 && unmatched && n[q]) {   // no counter to clear: hide the whole queue for this build
            const uint8_t* dir = pending_director();
            uintptr_t b = 0, e = 0;
            if (dir && mem_read(dir + qd.begin, &b, sizeof(b)) && mem_read(dir + qd.end, &e, sizeof(e)) && b && e > b) {
                g_hidden = HiddenQueue{ true, (uintptr_t)(dir + qd.end), b, e };
                mem_write((void*)g_hidden.end_at, &b, sizeof(b));
                log_line_lvl(LogLevel::Warn, "SPAWN", "!! %u of this peer's %s are not on the host -- the whole queue is hidden for this build", unmatched, qd.label);
            }
        }
        uint32_t missing = 0;
        for (uint32_t k = 0; k < hn[q]; ++k) if (!used[k]) ++missing;
        log_line("SPAWN", "%s aligned with the host's (%u entr%s): %u switched off here, %u field(s) set", qd.label, hn[q], hn[q] == 1 ? "y" : "ies", off_n, set_n);
        if (missing) log_line_lvl(LogLevel::Warn, "SPAWN", "!! the host has %u %s this peer's queue lacks -- they cannot be added here, the boards will differ", missing, qd.label);
    }
}

void unlocks_battle_build_done() {
    if (g_in_build) unlocks_build_stage_end("end of the build");
    pending_unhide();
    g_in_build = false;
}

namespace { uint32_t g_stage_no = 0; }

static void set_build_stream(uint64_t salt, const char* why, const char* what) {
    uint64_t* s = rng_global_stream();
    const uint64_t id = lockstep_battle_id();
    if (!s || !id) return;
    uint64_t before = 0;
    mem_read(s, &before, sizeof(before));
    uint64_t x = id ^ salt;
    uint64_t st[4];
    for (int i = 0; i < 4; ++i) st[i] = splitmix64(x);
    mem_write(s, st, sizeof(st));
    log_line("RNGTRACE", "%s %s: the stream was %016llx, set to %016llx", why, what, (unsigned long long)before, (unsigned long long)st[0]);
}

void unlocks_build_load_done() {
    if (!g_in_build || !net_active() || net_peer_count() < 2) return;
    set_build_stream(0x6D676D70646F6E65ull ^ ((uint64_t)g_load_no * 0x9E3779B97F4A7C15ull), "after load", "");
}

void unlocks_build_stage_end(const char* stage) {
    if (!g_in_build || !net_active() || net_peer_count() < 2) return;
    set_build_stream(0x6D676D7073746731ull ^ ((uint64_t)(++g_stage_no) * 0xD6E8FEB86659FD93ull), "build stage ends:", stage);
}

// A DEFINITION LOADED IN THE MIDDLE OF A BATTLE (2026-10-03, two halts at turn 18 of the same battle, and the first drift of the stream in it): loading a character definition draws a random cat NAME, and
// the draw depends on this peer's own save (the names it has used, the list it picks from), so the load consumes a different number of draws on each peer. The battle build was fenced against that
// long ago (above); a summon or a pickup made by an ability was not: the logs show both peers' stream equal before 'CharmedTomTom' and different before the next load ('RandomPickup'), the unit's creation
// key different on the two peers, and the pickup a DIFFERENT KIND on each ("unit 54 is another kind here than on the host") -- a state-only halt a turn later.
// The same cure: the load starts from a stream both peers derive from the stream they share at that moment, and when it is over the stream is set to another value derived the same way -- whatever
// the load drew in between does not reach the next roll. (Both values are a function of the shared stream, so the fight's own randomness is not frozen; and the host's board still sets the stream at
// every turn boundary as before.)
namespace { bool g_mid_in = false; uint32_t g_mid_no = 0; uint64_t g_mid_seed = 0; int g_mid_depth = 0; }

void unlocks_midbattle_load(const char* what) {
    if (g_mid_depth++ > 0) return;                                  // a load inside a load: the outer one's fence covers it
    g_mid_in = false;
    if (g_in_build || !net_active() || net_peer_count() < 2 || !lockstep_fight_up()) return;
    uint64_t* s = rng_global_stream();
    if (!s) return;
    uint64_t cur[4] = {};
    if (!mem_read(s, cur, sizeof(cur))) return;
    uint64_t x = cur[0] ^ (cur[1] * 0x9E3779B97F4A7C15ull) ^ (cur[2] * 0xBF58476D1CE4E5B9ull) ^ (cur[3] * 0x94D049BB133111EBull) ^ lockstep_battle_id() ^ 0x6D676D706D696431ull;      // NOT a running load counter: the two peers' counters can differ (a load one of them did not count), and a seed built on it parted the streams at the very first load (2026-10-03)
    ++g_mid_no;                                                    // only for the log
    g_mid_seed = x;
    uint64_t st[4];
    for (int i = 0; i < 4; ++i) st[i] = splitmix64(x);
    if (!mem_write(s, st, sizeof(st))) return;
    g_mid_in = true;
    if (g_mid_no <= 64) log_line("RNGTRACE", "mid-battle load of '%s': the stream was %016llx, the load starts from %016llx", what ? what : "?", (unsigned long long)cur[0], (unsigned long long)st[0]);
}

void unlocks_midbattle_load_done() {
    if (g_mid_depth > 0 && --g_mid_depth > 0) return;
    g_mid_depth = 0;
    if (!g_mid_in) return;
    g_mid_in = false;
    uint64_t* s = rng_global_stream();
    if (!s) return;
    uint64_t x = g_mid_seed ^ 0x6D676D706D696432ull;
    uint64_t st[4];
    for (int i = 0; i < 4; ++i) st[i] = splitmix64(x);
    mem_write(s, st, sizeof(st));
}

void unlocks_build_load() {
    if (!g_in_build || !net_active() || net_peer_count() < 2) return;
    uint64_t* s = rng_global_stream();
    const uint64_t id = lockstep_battle_id();
    if (!s || !id) return;
    uint64_t x = id ^ 0x6D676D706C6F6164ull ^ ((uint64_t)(++g_load_no) * 0x9E3779B97F4A7C15ull);
    uint64_t st[4];
    for (int i = 0; i < 4; ++i) st[i] = splitmix64(x);
    mem_write(s, st, sizeof(st));
}

bool unlocks_describe_cat(void* catdata, uint64_t& id, char* text, size_t cap, uint64_t& fp) {
    id = 0; fp = 0;
    if (!catdata || !text || !cap) return false;
    id = cat_id(catdata);
    describe_cat_inputs(catdata, text, cap, fp);
    return id != 0;
}

void unlocks_battle_build() {
    g_in_build = false;
    if (!net_active() || net_peer_count() < 2) return;
    uint64_t* s = rng_global_stream();
    const uint64_t id = lockstep_battle_id();
    if (!s || !id) {
        log_line_lvl(LogLevel::Warn, "SPAWN", "!! battle build without %s -- the stream is left as it is", s ? "a battle id" : "the shared stream");
        return;
    }
    uint64_t before[4] = {};
    mem_read(s, before, sizeof(before));
    uint64_t x = id ^ 0x6D676D70626C6431ull;
    uint64_t st[4];
    for (int i = 0; i < 4; ++i) st[i] = splitmix64(x);
    if (!mem_write(s, st, sizeof(st))) {
        log_line_lvl(LogLevel::Error, "SPAWN", "!! battle build: the shared stream could not be set");
        return;
    }
    g_in_build = true; g_load_no = 0; g_stage_no = 0;
    unlocks_build_enemies_begin();   // the director's queues and active modifiers: logged on both peers, aligned with the host's on a client (the weather's props are read from the first stage on)
    {   // the 20% bird of the build (0x35B54A): chance 0.2 only when MewDirector+0x70C == 0 and +0x6DC != 0 -- both are per-save, so say what this peer has
        const uintptr_t at = addr_of_data(D_MewDirectorPtr);
        const uint8_t* dir = nullptr;
        uint8_t f70c = 0xFF; int32_t d6dc = -1;
        if (at && mem_read((const void*)at, &dir, sizeof(dir)) && dir) { mem_read(dir + 0x70C, &f70c, 1); mem_read(dir + 0x6DC, &d6dc, 4); }
        log_line("SPAWN", "bird roll inputs: MewDirector+0x70C = %u, +0x6DC = %d -- the bird comes with 20%% only when the first is 0 and the second is not", (unsigned)f70c, d6dc);
    }
    log_line("SPAWN", "battle build: the shared stream %016llx -> %016llx, set from the battle id %016llx (the same on every peer)",
             (unsigned long long)before[0], (unsigned long long)st[0], (unsigned long long)id);
}

// Measured 2026-10-02 18:15: the stream agreed at node entry, the two peers then played DIFFERENT levels -- the level is picked inside
// MapScreen::EnterNode (sub_140394450), before the battle build. The pick now starts from a stream made of the node's own seed (MapNode+0x118,
// 32 bytes, the same on every peer since the map sync), or of the battle id when the caller passes no node.
static void publish_impl(const char* level, uint64_t level_node);
// THE HOST'S LEVEL DID NOT FIT THE CLIENT'S STRING (2026-10-03, the first battle played on two different levels): the pick returns its name in a std::string, and the client overwrites it in
// place -- which only works while the host's name fits the capacity the client's own pick left (31 for 'levels/alley/easy/101.lvl'); the host's 'levels/alley/rare/alley-elite12.lvl' is 36
// characters, so the old code gave up and each peer built its own level. The string is now REPLACED: a new one is built with the game's allocator, the old one is released the game's way
// (_Tidy_deallocate: frees a heap buffer and leaves an empty short string), and the new 32 bytes are put in its place. POD only: SEH frame.
static bool replace_game_string(void* out, const char* text) {
    const uintptr_t dtor_at = addr_of_call(C_GameStrDtor);
    const uintptr_t alloc_at = addr_of_call(C_GameStrAlloc);
    if (!out || !dtor_at || !alloc_at) return false;
    GameStr fresh;
    if (!make_key(fresh, text, (fn_alloc)alloc_at)) return false;   // built first: a failure leaves the old string untouched
    __try {
        ((void(__fastcall*)(void*))dtor_at)(out);
        memcpy(out, &fresh, sizeof(fresh));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

void unlocks_level_pick(void* kind, void* node, void* out, bool picked) {
    if (!net_active() || net_peer_count() < 2) return;
    uint64_t* s = rng_global_stream();
    if (!s) return;
    char what[48] = "?";
    if (kind) mem_read_std_string(kind, what, sizeof(what));
    uint64_t now = 0;
    mem_read(s, &now, sizeof(now));
    if (picked) {
        char level[96] = "?";
        if (out) mem_read_std_string(out, level, sizeof(level));
        log_line("SPAWN", "level pick (%s): '%s' -- stream after %016llx", what, level, (unsigned long long)now);
        const uint64_t id = lockstep_battle_id();
        if (!node || !out || !id || level[0] == '?') return;
        if (net_role() == NetRole::Host) { publish_impl(level, id); return; }
        // Client: the host's level for this battle (it may still be on its way: the receive thread fills a mailbox, wait for it).
        char want[64] = {};
        const ULONGLONG t0 = GetTickCount64();
        while (!net_host_level(id, want, sizeof(want)) && GetTickCount64() - t0 < tune::kLevelWaitMs) Sleep(5);
        if (!want[0]) {
            log_line_lvl(LogLevel::Warn, "SPAWN", "!! the host's level for battle %016llx did not arrive within %u ms -- this game's pick '%s' is kept", (unsigned long long)id, (unsigned)tune::kLevelWaitMs, level);
            room_sync_trouble("the host's level");
            return;
        }
        if (strcmp(want, level) == 0) { log_line("SPAWN", "level pick agrees with the host's ('%s', waited %llu ms)", level, (unsigned long long)(GetTickCount64() - t0)); return; }
        // Overwrite the returned std::string in place (MSVC layout: buffer or heap pointer, size at +0x10, capacity at +0x18).
        uint64_t sz = 0, cap = 0;
        const size_t n = strlen(want);
        if (!mem_read((const uint8_t*)out + 0x10, &sz, 8) || !mem_read((const uint8_t*)out + 0x18, &cap, 8)) {
            log_line_lvl(LogLevel::Error, "SPAWN", "!! the host's level '%s' differs from this game's '%s' and this game's string could not be read", want, level);
            return;
        }
        if (n > cap) {
            if (!replace_game_string(out, want)) {
                log_line_lvl(LogLevel::Error, "SPAWN", "!! the host's level '%s' differs from this game's '%s', does not fit its string (capacity %llu) and the string could not be replaced", want, level, (unsigned long long)cap);
                return;
            }
            char now_text[96] = "?";
            mem_read_std_string(out, now_text, sizeof(now_text));
            log_line_lvl(LogLevel::Warn, "SPAWN", "!! FORCED the level to the host's (the string was replaced: it did not fit capacity %llu): this game picked '%s', the host '%s', now '%s'", (unsigned long long)cap, level, want, now_text);
            return;
        }
        char* dst = cap > 15 ? nullptr : (char*)out;
        if (!dst) { uint64_t p = 0; if (!mem_read(out, &p, 8) || !p) return; dst = (char*)p; }
        const uint64_t nn = n;
        if (!mem_write(dst, want, n + 1) || !mem_write((uint8_t*)out + 0x10, &nn, 8)) {
            log_line_lvl(LogLevel::Error, "SPAWN", "!! the level string could not be overwritten");
            return;
        }
        log_line_lvl(LogLevel::Warn, "SPAWN", "!! FORCED the level to the host's: this game picked '%s', the host '%s'", level, want);
        return;
    }
    uint64_t seed[4] = {};
    const bool from_node = node && mem_read((const uint8_t*)node + 0x118, seed, sizeof(seed)) && (seed[0] | seed[1] | seed[2] | seed[3]);
    // EnterNode copies the node's seed into the stream first thing (0x140391CA3), so `now` should be that seed advanced by the draws made since --
    // and every per-save number the picker and the code before it read, so a difference between the two peers' logs names its source.
    {
        uint32_t type = 0;
        if (node) mem_read((const uint8_t*)node + 0x138, &type, sizeof(type));
        int32_t d6c8 = 0, d6cc = 0, d6d8 = 0, d6dc = 0;
        const uintptr_t at = addr_of_data(D_MewDirectorPtr);
        const uint8_t* dir = nullptr;
        if (at && mem_read((const void*)at, &dir, sizeof(dir)) && dir) {
            mem_read(dir + 0x6C8, &d6c8, 4); mem_read(dir + 0x6CC, &d6cc, 4); mem_read(dir + 0x6D8, &d6d8, 4); mem_read(dir + 0x6DC, &d6dc, 4);
        }
        log_line("SPAWN", "level pick (%s) inputs: node type %u, node seed %016llx.. | director +6C8 %d +6CC %d +6D8 %d +6DC %d | stream on entry %016llx%s",
                 what, type, (unsigned long long)seed[0], d6c8, d6cc, d6d8, d6dc, (unsigned long long)now,
                 from_node && now == seed[0] ? " (= the node seed: nothing drew since EnterNode copied it)" : "");
    }
    uint64_t x = from_node ? (seed[0] ^ (seed[1] * 0x9E3779B97F4A7C15ull) ^ (seed[2] << 1) ^ (seed[3] >> 1)) : lockstep_battle_id();
    if (!x) {
        log_line_lvl(LogLevel::Warn, "SPAWN", "!! level pick (%s) without a node seed or a battle id -- the stream is left as it is (%016llx)", what, (unsigned long long)now);
        return;
    }
    x ^= 0x6D676D706C766C31ull;
    uint64_t st[4];
    for (int i = 0; i < 4; ++i) st[i] = splitmix64(x);
    if (!mem_write(s, st, sizeof(st))) {
        log_line_lvl(LogLevel::Error, "SPAWN", "!! level pick: the shared stream could not be set");
        return;
    }
    log_line("SPAWN", "level pick (%s): the shared stream %016llx -> %016llx, set from %s %016llx (the same on every peer)", what,
             (unsigned long long)now, (unsigned long long)st[0], from_node ? "the node seed" : "the battle id",
             (unsigned long long)(from_node ? seed[0] : lockstep_battle_id()));
}

void unlocks_spawn_sort(void** first, void** last, bool sorted) {
    if (!net_active() || net_peer_count() < 2 || !first || last <= first || last - first > 16) return;
    const uint32_t n = (uint32_t)(last - first);
    void* cats[16] = {};
    if (!mem_read(first, cats, n * sizeof(void*))) return;
    uint64_t ids[16] = {};
    int keys[16] = {};
    char line[400] = {};
    int off = 0;
    for (uint32_t i = 0; i < n; ++i) {
        ids[i] = cat_id(cats[i]);
        int s[7] = {};
        keys[i] = cat_stats(cats[i], s) ? s[kSortStat] : -99999;
        append(line, sizeof(line), off, " %llx(SPD %d)", ids[i], keys[i]);
    }
    if (!sorted) {
        log_line("SPAWN", "the battle sorts the party's %u cat(s) for placement (by SPEED, highest first, ties keep this shuffled order); in:%s (stream %016llx)",
                 n, line, (unsigned long long)stream_s0());
        for (uint32_t i = 0; i < n; ++i) log_cat_inputs(cats[i]);
        return;
    }
    log_line("SPAWN", "the game's spawn order:%s (stream %016llx)", line, (unsigned long long)stream_s0());

    // The host's view of the same cats (sent with the unlock answers): a different key here means the machines disagree on a cat's stats.
    if (net_role() == NetRole::Client) {
        if (!host_lists.have || !host_lists.msg.n_spawn) {
            log_line_lvl(LogLevel::Warn, "SPAWN", "!! the host's spawn keys have not arrived -- the canonical order is still applied");
            room_sync_trouble("the host's spawn order");
        } else {
            const UnlocksMsg& m = host_lists.msg;
            for (uint32_t i = 0; i < m.n_spawn; ++i)
                for (uint32_t j = 0; j < n; ++j)
                    if (ids[j] == m.spawn_ids[i] && keys[j] != m.spawn_keys[i])
                        log_line_lvl(LogLevel::Warn, "SPAWN", "!! cat %016llx: the host sorts it on SPEED %d, this game computed %d -- the two machines disagree on this cat's stats",
                                     (unsigned long long)ids[j], (int)m.spawn_keys[i], keys[j]);
        }
    }

    // The canonical order (see predict_spawn), on EVERY peer.
    UnlocksMsg canon{};
    if (!predict_spawn(canon) || canon.n_spawn != n) {
        log_line_lvl(LogLevel::Warn, "SPAWN", "!! the party list could not be read (or holds another number of cats than the battle sorted: %u) -- the game's order is kept", n);
        return;
    }
    void* want[16] = {};
    bool used[16] = {};
    char want_line[400] = {};
    off = 0;
    for (uint32_t i = 0; i < n; ++i) {
        append(want_line, sizeof(want_line), off, " %llx(SPD %d)", canon.spawn_ids[i], canon.spawn_keys[i]);
        int at = -1;
        for (uint32_t j = 0; j < n && at < 0; ++j) if (!used[j] && ids[j] == canon.spawn_ids[i]) at = (int)j;
        if (at < 0) {
            log_line_lvl(LogLevel::Warn, "SPAWN", "!! cat %016llx of the party list is not among the cats the battle sorted -- the game's order is kept",
                         (unsigned long long)canon.spawn_ids[i]);
            return;
        }
        used[at] = true;
        want[i] = cats[at];
    }
    if (net_role() == NetRole::Host) {
        bool same = predicted.n == n;
        for (uint32_t i = 0; same && i < n; ++i) same = predicted.ids[i] == canon.spawn_ids[i];
        if (!same) log_line_lvl(LogLevel::Warn, "SPAWN", "!! the canonical order now differs from the one sent to the clients (%u cat(s) sent) -- a cat's speed changed in between", predicted.n);
    }
    if (memcmp(want, cats, n * sizeof(void*)) == 0) {
        log_line("SPAWN", "the game's order is already the canonical one (speed, then the party list)");
        return;
    }
    if (!mem_write(first, want, n * sizeof(void*))) {
        log_line_lvl(LogLevel::Error, "SPAWN", "!! the spawn order could not be rewritten");
        return;
    }
    log_line_lvl(LogLevel::Warn, "SPAWN", "!! spawn order set to the canonical one (speed, then the party list):%s -- the game's shuffle gave%s", want_line, line);
}

bool unlocks_window_active() {
    if (!win.on || g_own_read) return false;
    if (GetTickCount64() - win.at > kWindowMaxMs) { unlocks_window_close("timed out"); return false; }
    return true;
}

static void publish_impl(const char* level, uint64_t level_node);
void unlocks_publish() { publish_impl(nullptr, 0); }

static void publish_impl(const char* level, uint64_t level_node) {
    if (!net_active() || net_role() != NetRole::Host) return;
    UnlocksMsg m;
    if (!snapshot(m)) { static bool said = false; if (!said) { said = true; log_line_lvl(LogLevel::Warn, "UNLOCK", "could not read this game's unlock answers -- the clients keep their own"); } return; }
    if (!predict_spawn(m)) {
        m.n_spawn = 0;
        log_line_lvl(LogLevel::Warn, "SPAWN", "could not work out this party's spawn order -- the clients keep their own");
    }
    if (level && level[0] && level_node) {
        pending_fill(m);
        m.build_info = 1;
        m.level_node = level_node;
        m.n_level_name = (uint8_t)strnlen(level, sizeof(m.level_name) - 1);
        memcpy(m.level_name, level, m.n_level_name);
        if (!net_send_unlocks(m)) return;
        log_line("SPAWN", "level pick: the host's level '%s' sent to the clients (battle %016llx)", level, (unsigned long long)level_node);
        return;
    }
    if (!net_send_unlocks(m)) return;
    props_publish();
    predicted.n = m.n_spawn;
    for (uint32_t i = 0; i < m.n_spawn; ++i) { predicted.ids[i] = m.spawn_ids[i]; predicted.keys[i] = m.spawn_keys[i]; }
    if (m.n_spawn) {
        char line[400] = {};
        int off = 0;
        for (uint32_t i = 0; i < m.n_spawn; ++i) append(line, sizeof(line), off, " %llx(SPD %d)", m.spawn_ids[i], m.spawn_keys[i]);
        log_line("SPAWN", "the host's party will spawn in this order (sent to the clients):%s", line);
    }
    static uint32_t lastA = 0, lastP = 0, lastL = 0, lastB = 0; static uint8_t lastI[16] = {}; static bool first = true;
    if (first || lastA != m.abilities || lastP != m.passives || lastL != m.levels || lastB != m.bosses || memcmp(lastI, m.items, sizeof(lastI)) != 0) {
        first = false; lastA = m.abilities; lastP = m.passives; lastL = m.levels; lastB = m.bosses; memcpy(lastI, m.items, sizeof(lastI));
        unsigned u[5] = {};
        for (int li = 0; li < 5; ++li) for (int i = 0; i < list_size((UnlockList)li); ++i) if (bit_of(m, (UnlockList)li, i)) ++u[li];
        unsigned nonzero = 0;
        for (int i = 0; i < m.n_events; ++i) if (m.events[i]) ++nonzero;
        log_line("UNLOCK", "the host's unlock answers sent to the clients (epoch %u): abilities %u/%d, passives %u/%d, items %u/%d, level groups %u/%d, bosses %u/%d unlocked; %u of %d event properties set",
                 m.epoch, u[0], unlock_lists::kAbilityCount, u[1], unlock_lists::kPassiveCount, u[2], unlock_lists::kItemCount, u[3], unlock_lists::kLevelGroupCount, u[4], unlock_lists::kBossCount, nonzero, (int)m.n_events);
    }
}

void unlocks_props_on_message(uint8_t from, const PropsMsg& m) {
    if (net_role() != NetRole::Client || from != kHostPeer) return;
    if (m.epoch != hprops.epoch || m.first == 0) {
        if (m.first != 0) return;                         // the middle of a table that never started here
        hprops.have = false; hprops.epoch = m.epoch; hprops.total = m.total; hprops.got = 0;
    }
    if (m.first != hprops.got || m.total != hprops.total || hprops.got + m.count > HostProps::kMax) return;
    for (uint32_t i = 0; i < m.count; ++i) { hprops.hash[hprops.got + i] = m.hash[i]; hprops.value[hprops.got + i] = m.value[i]; }
    hprops.got += m.count;
    if (hprops.got == hprops.total && !hprops.have) {
        hprops.have = true;
        static uint32_t last = ~0u;
        if (last != hprops.total) { last = hprops.total; log_line("UNLOCK", "the host's whole save-property table arrived: %u properties", hprops.total); }
    }
}

void unlocks_on_message(uint8_t from, const UnlocksMsg& m) {
    if (net_role() != NetRole::Client || from != kHostPeer || !valid_unlocks(m)) return;
    if (m.n_abilities != unlock_lists::kAbilityCount || m.n_passives != unlock_lists::kPassiveCount ||
        m.n_items != unlock_lists::kItemCount || m.n_levels != unlock_lists::kLevelGroupCount || m.n_bosses != unlock_lists::kBossCount || m.n_events != event_keys::kCount) {
        log_line_lvl(LogLevel::Warn, "UNLOCK", "!! the host's unlock lists have other sizes (%u/%u/%u/%u, here %d/%d/%d/%d) -- another game data version; they are ignored",
                     m.n_abilities, m.n_passives, m.n_items, m.n_levels, unlock_lists::kAbilityCount, unlock_lists::kPassiveCount, unlock_lists::kItemCount, unlock_lists::kLevelGroupCount);
        host_lists.have = false;
        return;
    }
    const bool fresh = !host_lists.have;
    host_lists.have = true; host_lists.epoch = m.epoch; host_lists.msg = m;
    if (fresh) log_line("UNLOCK", "the host's unlock answers arrived (epoch %u)", m.epoch);
}

void unlocks_window_open(const char* why, bool event) {
    cls = ClassState{};                 // a new window: the class list answers with the host's again until its fight is over
    if (net_role() != NetRole::Client) return;
    if (!host_lists.have) {
        log_line_lvl(LogLevel::Warn, "UNLOCK", "%s but the host's unlock answers have not arrived -- this save's own lists are used (%s)", event ? "an event is being drawn" : "a battle is being built", why ? why : "");
        room_sync_trouble("the host's unlock answers");
        return;
    }
    win = Window{};
    win.on = true; win.event = event; win.at = GetTickCount64();
    _snprintf_s(win.why, sizeof(win.why), _TRUNCATE, "%s", why ? why : "");
    log_line("UNLOCK", "window OPEN (%s): until the map is ready again (the %s) the unlock checks and the save properties answer with the host's", win.why,
             event ? "event" : "battle and its rewards");
}

void unlocks_window_close(const char* why) {
    if (!win.on) return;
    win.on = false;
    log_line("UNLOCK", "window CLOSED (%s) after %llu ms: checks ability %u, passive %u, item %u, level group %u, boss %u; answered differently from this save: %u, %u, %u, %u, %u; event properties read %u, differing %u",
             why ? why : "", (unsigned long long)(GetTickCount64() - win.at),
             win.checks[0], win.checks[1], win.checks[2], win.checks[3], win.checks[4], win.differing[0], win.differing[1], win.differing[2], win.differing[3], win.differing[4], win.props, win.props_differing);
}

void unlocks_tick() {
    if (!win.on || win.left_map || !lockstep_battle_ready()) return;
    win.left_map = true;
    log_line("UNLOCK", "the battle is built (%u ability, %u passive, %u item, %u level group, %u boss check(s) so far) -- the window stays open until the map is ready again",
             win.checks[0], win.checks[1], win.checks[2], win.checks[3], win.checks[4]);
}

void unlocks_on_map(bool ready) {
    if (g_wswap.on && ready && GetTickCount64() - g_wswap.at_ms > 3000 && !lockstep_fight_up()) weather_restore();   // the map does not tick during a fight, so "ready again" is the signal
    if (!win.on) return;
    if (!ready) { win.left_map = true; return; }
    if (win.left_map) unlocks_window_close(win.event ? "the event node was left -- the map is ready again" : "the battle is over -- the map is ready again");
}

bool event_window_on() {
    if (!win.on || g_own_read || !host_lists.have) return false;
    if (GetTickCount64() - win.at > kWindowMaxMs) { unlocks_window_close("timed out"); return false; }
    return true;
}

// A save property read inside the window that the host's answers do not cover: this save's own value is used. Each name once per session, so the
// log says which per-save numbers a battle or an event reads that could still differ between the players.
void note_uncovered(const char* key, int64_t local) {
    static uint64_t seen[128];
    static unsigned n = 0;
    uint64_t h = 1469598103934665603ull;
    for (const char* p = key; *p; ++p) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    for (unsigned i = 0; i < n; ++i) if (seen[i] == h) return;
    if (n >= 128) return;
    seen[n++] = h;
    log_line("UNLOCK", "save property '%s' read during the %s is NOT among the host's answers -- this save's value %lld is used",
             key, win.event ? "event" : "battle", (long long)local);
}

// The property getter's answer for one of the properties the events read: the HOST's value, with every difference from this save's own on the record.
bool event_property_answer(const char* key, int64_t local, int64_t& value) {
    if (!event_window_on()) return false;
    for (int i = 0; i < event_keys::kCount; ++i) {
        if (strcmp(event_keys::kKeys[i], key) != 0) continue;
        const int32_t host = host_lists.msg.events[i];
        ++win.props;
        // THE HOST'S VALUE, BUT NOT FROZEN (2026-10-03, "Jack": donate 5 coins, `increment_legacy_counter` x5, then `conditional_reward` by counter_minimum 25). The host reads
        // its live counter (20 -> 25 -> the item tier); this peer kept answering the snapshot 20, every increment re-wrote 21, and the reward tier came out "grandma does not like you"
        // here. The events change a property by reading it (our answer) and writing back a value computed from it, so a value in this save that is not the one last seen was written by
        // the event: it is built on the host's numbers, and from then on it is what the reads answer.
        if (!win.prop_have[i]) {
            win.prop_have[i] = true;
            win.prop_last[i] = local;
            win.prop_virt[i] = host;
            if ((int64_t)host != local) {
                ++win.props_differing;
                if (win.logged < kMaxDifferenceLines) {
                    ++win.logged;
                    log_line("UNLOCK", "event property '%s': the host has %d, this save has %lld -- answered with the host's", key, (int)host, (long long)local);
                }
            }
        } else if (local != win.prop_last[i]) {
            win.prop_last[i] = local;
            win.prop_virt[i] = local < -2000000000LL ? -2000000000 : local > 2000000000LL ? 2000000000 : (int32_t)local;
            log_line("UNLOCK", "event property '%s' was written by the event (now %lld, built on the host's %d) -- the reads follow it", key, (long long)local, (int)host);
        }
        value = win.prop_virt[i];
        return true;
    }
    // A property that is not one of the 55 but IS in the host's whole table (proto 79): the host's value, followed through the event's own writes like the others. Not the map flags (they have their
    // own rule at map generation), the run's own markers, and the level-up counters (the level-up that follows a fight draws from THIS save's own numbers).
    if (hprops.have && hprops.got == hprops.total && strncmp(key, "mapflag_", 8) != 0 && strncmp(key, "SeenLevelup", 11) != 0 && strcmp(key, "on_adventure") != 0 &&
        strcmp(key, "departed_first_real_adventure") != 0) {
        const uint32_t h = prop_key_hash(key);
        int32_t host = 0;
        if (hprops.lookup(h, host)) {
            unsigned d = 0;
            for (; d < win.dyn_n; ++d) if (win.dyn_hash[d] == h) break;
            if (d == win.dyn_n) {
                if (win.dyn_n >= Window::kDyn) { note_uncovered(key, local); return false; }
                win.dyn_hash[d] = h; win.dyn_last[d] = local; win.dyn_virt[d] = host; ++win.dyn_n; ++win.props;
                if ((int64_t)host != local) {
                    ++win.dyn_differing; ++win.props_differing;
                    if (win.logged < kMaxDifferenceLines) { ++win.logged; log_line("UNLOCK", "save property '%s' (the host's whole table): the host has %d, this save has %lld -- answered with the host's", key, (int)host, (long long)local); }
                }
            } else if (local != win.dyn_last[d]) {
                win.dyn_last[d] = local;
                win.dyn_virt[d] = local < -2000000000LL ? -2000000000 : local > 2000000000LL ? 2000000000 : (int32_t)local;
                log_line("UNLOCK", "save property '%s' was written (now %lld, built on the host's %d) -- the reads follow it", key, (long long)local, (int)host);
            }
            value = win.dyn_virt[d];
            return true;
        }
    }
    note_uncovered(key, local);
    return false;
}

void unlocks_property_set(const void* name, int value) {
    if (!event_window_on()) return;
    char key[96] = {};
    if (!name || !mem_read_std_string((const uint8_t*)name, key, sizeof(key))) return;
    for (int i = 0; i < event_keys::kCount; ++i) {
        if (strcmp(event_keys::kKeys[i], key) != 0) continue;
        win.prop_have[i] = true;
        win.prop_last[i] = value;
        win.prop_virt[i] = value;
        log_line("UNLOCK", "event property '%s' is being written by the event: %d (built on the host's %d) -- the reads follow it", key, value, (int)host_lists.msg.events[i]);
        return;
    }
    const uint32_t h = prop_key_hash(key);
    for (unsigned d = 0; d < win.dyn_n; ++d) if (win.dyn_hash[d] == h) { win.dyn_last[d] = value; win.dyn_virt[d] = value; return; }
}

// ============================================================================================================================================
// THE CLASS LIST IN A BATTLE (2026-10-04). Only while the fight is on: the window opens at the node and the override ends when the fight does (the character list the roster was taken from is gone:
// lockstep_fight_up false after having been true), so the level-up that follows draws from THIS save's own classes. An event node never has a fight, so there it lasts as long as the window does,
// like the ability checks.
// ============================================================================================================================================
// THE FIGHT'S END, FOR EVERY UNLOCK ANSWER THE WINDOW GIVES (2026-10-04): the checks of abilities, passives, items, level groups and bosses and the class list answer with the host's only while the
// fight is on. Seen from the first call after it: the character list the roster was taken from was up at some call and is gone at a later one. From then on this save answers for itself, so the
// level-up and the rewards that follow use THIS player's own unlocks. (Save properties keep following the host's table until the map is ready: the level-up's own counters are excluded there.)
// An event node never has a fight, so there the window lasts as before.
bool unlocks_fight_released() {
    if (!lockstep_active()) return false;
    const uint64_t battle = lockstep_battle_id();
    if (cls.battle != battle) { cls = ClassState{}; cls.battle = battle; }
    const bool up = lockstep_fight_up();
    if (up) cls.seen_up = true;
    const bool won = up && lockstep_enemies_all_down();       // the level-up pool is built in the frame of the last kill, while the list is still there
    if ((won || (!up && cls.seen_up)) && !cls.released) {
        cls.released = true;
        log_line("UNLOCK", "the unlock answers (abilities, passives, items, level groups, bosses, classes) are this save's own again: the fight is over (%s; the level-up and the rewards that follow use this save's unlocks)",
                 won ? "every enemy is down" : "the battle's character list is gone");
    }
    return cls.released;
}

// --- the combat speed (proto 82) --------------------------------------------------------------------------------------------------------------------------
namespace {
float g_host_speed = 0.0f;           // client: what the host told us (0 = nothing yet)
float g_sent_speed = 0.0f;           // host: what was last sent
uint8_t g_sent_peers = 0;            // host: how many players there were then (a player who joined must be told again)
bool g_speed_said = false;
}
bool settings_override(const char* key, float own, float& out) {
    if (!key || strcmp(key, "combat_speed") != 0) return false;
    if (!net_active() || net_peer_count() < 2) { g_host_speed = 0.0f; g_sent_speed = 0.0f; g_sent_peers = 0; g_speed_said = false; return false; }
    if (net_role() == NetRole::Host) {
        const uint8_t peers = net_peer_count();
        if (own != g_sent_speed || peers != g_sent_peers) {
            SettingMsg m; m.id = kSettingCombatSpeed; m.value = own;
            if (net_send_setting(m)) {
                g_sent_speed = own; g_sent_peers = peers;
                log_line("SETTING", "the host's combat speed %.2f is sent to the other players (their game uses it, so every animation takes the same time)", (double)own);
            }
        }
        return false;
    }
    if (g_host_speed <= 0.0f) return false;
    if (!g_speed_said || own != g_host_speed) {
        if (!g_speed_said) log_line("SETTING", "this game uses the HOST's combat speed %.2f while in the room (this player's own setting is %.2f and is not changed)", (double)g_host_speed, (double)own);
        g_speed_said = true;
    }
    out = g_host_speed;
    return true;
}

void settings_on_message(uint8_t from, const SettingMsg& m) {
    if (net_role() == NetRole::Host || from != kHostPeer || m.id != kSettingCombatSpeed) return;
    if (m.value != g_host_speed) log_line("SETTING", "the host's combat speed is now %.2f", (double)m.value);
    g_host_speed = m.value;
}

void unlocks_level_screen_opens() {
    if (!lockstep_active() || !lockstep_battle_ready()) return;    // an event's level-up is not a fight's reward: the window goes on there
    const uint64_t battle = lockstep_battle_id();
    if (cls.battle != battle) { cls = ClassState{}; cls.battle = battle; }
    if (cls.released) return;
    cls.released = true;
    log_line("UNLOCK", "the unlock answers (abilities, passives, items, level groups, bosses, classes) are this save's own again: the fight is over (the level-up screen of its reward is being built; the level-up uses this save's unlocks)");
}

void unlocks_classes_after(void* out_vector, bool with_colorless) {
    if (!out_vector || !lockstep_active()) return;
    const int f = with_colorless ? 1 : 0;
    const uint64_t battle = lockstep_battle_id();
    if (cls.battle != battle) { cls = ClassState{}; cls.battle = battle; }
    GameVec* o = (GameVec*)out_vector;
    char mine[kClassMax][kClassLen] = {};
    uint8_t n_mine = 0;
    const bool have_mine = read_class_vector(*o, mine, n_mine);
    unlocks_fight_released();                                  // the fight's end, seen from here (see above)
    // One line per distinct list per flag per battle, on BOTH peers: what this peer's save lists, what the game is given.
    auto say = [&](const char* what, const std::string& given) {
        uint64_t h = 1469598103934665603ULL;
        for (const char* c = what; *c; ++c) { h ^= (uint8_t)*c; h *= 1099511628211ULL; }
        for (char c : given) { h ^= (uint8_t)c; h *= 1099511628211ULL; }
        if (cls.logged[f] == h) return;
        cls.logged[f] = h;
        log_line("UNLOCK", "class list (%s Colorless) of battle %016llx: this save [%s] -- %s", with_colorless ? "with" : "without", (unsigned long long)battle,
                 have_mine ? class_text(mine, n_mine).c_str() : "unreadable", what);
    };
    if (!unlocks_window_active() || !host_lists.have || host_lists.msg.n_classes[f] == 255) { say("kept", have_mine ? class_text(mine, n_mine) : std::string()); return; }
    if (cls.released) { say("kept (the fight is over)", have_mine ? class_text(mine, n_mine) : std::string()); return; }
    const uint8_t n = host_lists.msg.n_classes[f];
    const std::string host = class_text(host_lists.msg.classes[f], n);
    if (have_mine && n_mine == n && host == class_text(mine, n_mine)) { say("equals the host's", host); return; }
    for (uint8_t i = 0; i < n; ++i) if (strlen(host_lists.msg.classes[f][i]) > 15) { say("kept: a host name too long for the game's short strings", host); return; }
    auto alloc = (fn_alloc)addr_of_call(C_GameStrAlloc);
    const size_t bytes = (size_t)n * 0x20;
    void* buf = (n && alloc) ? call_alloc(alloc, bytes) : nullptr;
    if (n && !buf) { say("kept: the game's allocator is not there", host); return; }
    if (n) {
        memset(buf, 0, bytes);
        for (uint8_t i = 0; i < n; ++i) {
            uint8_t* s = (uint8_t*)buf + (size_t)i * 0x20;
            const size_t len = strlen(host_lists.msg.classes[f][i]);
            memcpy(s, host_lists.msg.classes[f][i], len + 1);
            const uint64_t sz = len, cap = 15;
            memcpy(s + 0x10, &sz, 8); memcpy(s + 0x18, &cap, 8);
        }
    }
    o->b = buf; o->e = (uint8_t*)buf + bytes; o->c = o->e;       // the old buffer is the game's; it is left where it is
    say("REPLACED by the host's", host);
}

// ============================================================================================================================================
// THE UNLOCK-QUERY RECORD (2026-10-04). Every unlock check and save-property read a battle makes, on BOTH peers: what this save said, what the game was told, and where the shared stream stood.
// A battle's enemy that picks from "the abilities unlocked" is a draw plus a filter on these answers, so two peers that were told different things (or asked a different number of times) draw
// differently -- 2026-10-04 b521766e: the boss's third action was Suplex on the host and a second AOE spell on the client, 13 ability checks answered by the client only. Three kinds of line:
//   UQ NEW   the first time a (kind, name, this save, answer) is seen in a battle: the name and the stream prefix
//   UQ       after each action: how many queries it made, per kind, a digest of the whole sequence and the first names -- two peers' lines for the same turn and action compare directly
// ============================================================================================================================================
namespace {
struct UQ {
    uint64_t battle = 0;
    unsigned n = 0, kind[6] = {};                  // queries since the last flush; per kind (ability, passive, item, level, boss, property)
    uint64_t digest = 0;
    uint32_t first_rng = 0;
    char head[160] = {};
    uint64_t seen[768] = {}; unsigned seen_n = 0;
    unsigned lines = 0;
    uint32_t seq = 0;                              // the flush number in this battle: the same on both peers (the flush points are the actions)
} uq;
const char* const kUqKinds[6] = { "ability", "passive", "item", "level", "boss", "prop" };

uint64_t uq_hash(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}
void uq_note(unsigned kind, const char* name, int64_t local, int64_t answer, bool listed) {
    // Only the FIGHT's own queries: the battle is snapshotted, its character list is live and an enemy is still standing. lockstep_in_battle() alone stays true from one battle's snapshot to the next, so the
    // host's own publishing of its answers (twice the lists, at node entry) and the client's map and node reads were counted into the next battle's first flush and reported as a mismatch (live test
    // 2026-10-04: 502 against 283 queries, both battles' hashes agreed). The same state on both peers, so the same gate on both. The level-up's queries after the last kill are not the fight's either.
    if (!lockstep_in_battle() || !lockstep_fight_up() || lockstep_enemies_all_down()) return;
    if (cls.released && cls.battle == lockstep_battle_id()) return;      // the fight's reward (the level-up pool) is not the fight's own
    const uint64_t battle = lockstep_battle_id();
    if (battle != uq.battle) { uq = UQ{}; uq.battle = battle; }
    const uint64_t* stream = rng_global_stream();
    uint64_t r0 = 0;
    if (stream) mem_read(stream, &r0, sizeof(r0));
    if (!uq.n) uq.first_rng = (uint32_t)r0;
    ++uq.n; ++uq.kind[kind < 6 ? kind : 5];
    // SAVE PROPERTY READS ARE LOGGED AND COUNTED, BUT NOT IN THE DIGEST (2026-10-04, a false UQ MISMATCH in a live test): the host read 21 properties (on_adventure, the mapflag_*Unlocked family,
    // departed_first_real_adventure) in the middle of a battle and the client none -- the set the game's own save routine reads, which runs when each peer's own timer says so and has nothing to do with
    // the fight. No battle has ever been seen to read a property in its simulation, so a difference in WHEN a peer reads one is not a divergence; the answers a read would get are the same table on both.
    if (kind != 5) {
        uint64_t h = uq.digest ? uq.digest : 1469598103934665603ULL;
        h = uq_hash(h, &kind, sizeof(kind));
        h = uq_hash(h, name, strlen(name));
        h = uq_hash(h, &answer, sizeof(answer));
        uq.digest = h;
    }
    const size_t used = strlen(uq.head);
    if (used < sizeof(uq.head) - 40 && uq.n <= 6) _snprintf_s(uq.head + used, sizeof(uq.head) - used, _TRUNCATE, " %s:%s=%lld", kUqKinds[kind < 6 ? kind : 5], name, (long long)answer);
    // the first sighting of this exact answer in this battle
    uint64_t key = 1469598103934665603ULL;
    key = uq_hash(key, &kind, sizeof(kind)); key = uq_hash(key, name, strlen(name)); key = uq_hash(key, &local, sizeof(local)); key = uq_hash(key, &answer, sizeof(answer));
    for (unsigned i = 0; i < uq.seen_n; ++i) if (uq.seen[i] == key) return;
    if (uq.seen_n < sizeof(uq.seen) / sizeof(uq.seen[0])) uq.seen[uq.seen_n++] = key;
    if (uq.lines >= 400) return;
    ++uq.lines;
    log_line_lvl(LogLevel::Trace, "UQ", "NEW %s '%s': this save %lld, answered %lld%s | rng %08x | turn %u",
                 kUqKinds[kind < 6 ? kind : 5], name, (long long)local, (long long)answer, local != answer ? " (DIFFERS)" : "", (unsigned)r0, (unsigned)log_turn());
}
} // namespace

void unlockq_query(UnlockList list, const char* name, bool local, bool answer) { uq_note((unsigned)list, name, local ? 1 : 0, answer ? 1 : 0, true); }
void unlockq_prop(const char* key, int64_t local, int64_t answer) { uq_note(5, key, local, answer, true); }

// --- the digests of the two peers, side by side (MSG_UQD) -------------------------------------------------------------------------------------
namespace {
struct UqdRec { UqdMsg m; bool have = false; char head[160] = {}; };
UqdRec uqd_mine[64], uqd_peer[kMaxPeers][64];
uint64_t uqd_battle = 0;
uint32_t uqd_compared = 0, uqd_mismatched = 0;
bool uqd_shared = false;

void uqd_reset(uint64_t battle) {
    for (auto& r : uqd_mine) r = UqdRec{};
    for (auto& row : uqd_peer) for (auto& r : row) r = UqdRec{};
    uqd_battle = battle; uqd_compared = uqd_mismatched = 0; uqd_shared = false;
}
void uqd_check(uint32_t seq, uint8_t from) {
    UqdRec& a = uqd_mine[seq % 64];
    UqdRec& b = uqd_peer[from][seq % 64];
    if (!a.have || !b.have || a.m.seq != seq || b.m.seq != seq || a.m.battle != b.m.battle) return;
    b.have = false;                                 // compared once
    ++uqd_compared;
    bool same = a.m.digest == b.m.digest;                          // properties (kind 5) are not part of it: see uq_note
    for (int k = 0; k < 5; ++k) if (a.m.kind[k] != b.m.kind[k]) same = false;
    if (same) return;
    ++uqd_mismatched;
    log_line_lvl(LogLevel::Error, "UQ", "!! UQ MISMATCH battle %016llx turn %u after action %u (flush #%u): THIS peer made %u quer%s [ability %u, passive %u, item %u, level %u, boss %u, prop %u] digest %08x "
                 "(first at rng %08x:%s) | peer %u made %u [ability %u, passive %u, item %u, level %u, boss %u, prop %u] digest %08x (first at rng %08x) -- the two saves were asked different things "
                 "(or answered differently) in this action", (unsigned long long)a.m.battle, a.m.turn, a.m.action, a.m.seq, a.m.n, a.m.n == 1 ? "y" : "ies", a.m.kind[0], a.m.kind[1], a.m.kind[2], a.m.kind[3],
                 a.m.kind[4], a.m.kind[5], a.m.digest, a.m.first_rng, a.head, (unsigned)from, b.m.n, b.m.kind[0], b.m.kind[1], b.m.kind[2], b.m.kind[3], b.m.kind[4], b.m.kind[5], b.m.digest, b.m.first_rng);
    if (!uqd_shared) { uqd_shared = true; lockstep_share_log("an unlock-query mismatch"); }
}
// This peer's digest for the flush that is happening now: kept, and sent to the others whether or not there were any queries.
void uq_send_digest(uint32_t turn, uint32_t action) {
    if (!net_active() || net_peer_count() < 2 || !lockstep_active()) return;
    const uint64_t battle = lockstep_battle_id();
    if (battle != uq.battle) { uq = UQ{}; uq.battle = battle; }
    if (battle != uqd_battle) uqd_reset(battle);
    UqdMsg m;
    m.battle = battle; m.seq = ++uq.seq; m.turn = turn; m.action = action; m.n = uq.n;
    for (int k = 0; k < 6; ++k) m.kind[k] = uq.kind[k];
    m.digest = (uint32_t)uq.digest; m.first_rng = uq.first_rng;
    UqdRec& r = uqd_mine[m.seq % 64];
    r.m = m; r.have = true; strncpy_s(r.head, sizeof(r.head), uq.head, _TRUNCATE);
    net_send_uqd(m);
    for (uint8_t p = 0; p < kMaxPeers; ++p) uqd_check(m.seq, p);
}
} // namespace

void unlockq_on_peer(uint8_t from, const UqdMsg& m) {
    if (from >= kMaxPeers || !lockstep_active() || m.battle != lockstep_battle_id()) return;
    if (m.battle != uqd_battle) uqd_reset(m.battle);
    UqdRec& r = uqd_peer[from][m.seq % 64];
    r.m = m; r.have = true;
    uqd_check(m.seq, from);
}
void unlockq_stats(uint32_t& compared, uint32_t& mismatched) {
    compared = uqd_battle == lockstep_battle_id() ? uqd_compared : 0;
    mismatched = uqd_battle == lockstep_battle_id() ? uqd_mismatched : 0;
}

// After each action and at each actor's start: one line for what the queries since the last line were. Quiet when there were none.
void unlockq_flush(uint32_t turn, uint32_t actions_done) {
    uq_send_digest(turn, actions_done);                 // before anything is reset: the other peers get this action's digest, and it is compared with theirs
    if (!uq.n) return;
    log_line("UQ", "turn %u after action %u: %u quer%s (ability %u, passive %u, item %u, level %u, boss %u, prop %u) digest %08x, first at rng %08x:%s", (unsigned)turn, (unsigned)actions_done,
             uq.n, uq.n == 1 ? "y" : "ies", uq.kind[0], uq.kind[1], uq.kind[2], uq.kind[3], uq.kind[4], uq.kind[5], (unsigned)uq.digest, (unsigned)uq.first_rng, uq.head);
    const uint64_t b = uq.battle;
    unsigned seen_n = uq.seen_n; uint64_t seen[768]; memcpy(seen, uq.seen, sizeof(seen)); const unsigned lines = uq.lines;
    const uint32_t keep_seq = uq.seq;
    uq = UQ{}; uq.battle = b; uq.seen_n = seen_n; memcpy(uq.seen, seen, sizeof(seen)); uq.lines = lines; uq.seq = keep_seq;
}

bool unlocks_window_answer(UnlockList list, const char* name, bool local) {
    const int li = (int)list;
    if (unlocks_fight_released()) { unlockq_query(list, name, local, local); return local; }     // the fight is over: this save answers for itself
    ++win.checks[li];
    if (!host_lists.have) { unlockq_query(list, name, local, local); return local; }
    const int idx = find_name(list, name);
    if (idx < 0) { unlockq_query(list, name, local, local); return local; }                          // not on a blacklist: available to everybody, nothing to unify
    const bool host = bit_of(host_lists.msg, list, idx);
    unlockq_query(list, name, local, host);
    ++win.overridden[li];
    if (host != local) {
        ++win.differing[li];
        if (win.logged < kMaxDifferenceLines) {
            ++win.logged;
            log_line("UNLOCK", "%s '%s': the host says %s, this save says %s -- answered with the host's",
                     kListNames[li], name, host ? "UNLOCKED" : "locked", local ? "unlocked" : "locked");
        } else if (win.logged == kMaxDifferenceLines) {
            ++win.logged;
            log_line("UNLOCK", "(more answers differ from this save; only the first %u are listed -- the summary at the end counts all)", kMaxDifferenceLines);
        }
    }
    return host;
}

} // namespace mgmp
