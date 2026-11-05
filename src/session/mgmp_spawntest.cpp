// mgmp_spawntest.cpp -- see mgmp_spawntest.h.
#include "mgmp_spawntest.h"

#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "mgmp_addresses.h"
#include "mgmp_config.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_resolve.h"
#include "mgmp_rtti.h"
#include "mgmp_tuning.h"

namespace mgmp {
namespace {

// The battle's character list, reached from TurnControl -- the same chain mgmp_lockstep resolves (see kHolder_List there).
constexpr uintptr_t kTC_Scene    = 0x18;
constexpr uintptr_t kScene_Sub   = 0x08;    // the World
constexpr uintptr_t kSub_Holder  = 0x20;
constexpr uintptr_t kHolder_List = 0x1F90;  // type 0x1F9 (Character) * 16: the filtered copy of the world's type table
constexpr uintptr_t kList_Count  = 12;
constexpr uintptr_t kList_Data   = 16;
constexpr uintptr_t kTC_Actor    = 0x68;    // the character whose turn it is (the transform swaps it when it replaces that one)

constexpr uintptr_t kChar_Entity  = 0x18;   // the parent Entity: its children are the unit's components
constexpr uintptr_t kChar_World   = 0x20;
constexpr uintptr_t kChar_TObj    = 0x60;
constexpr uintptr_t kChar_Brain   = 0x68;
constexpr uintptr_t kChar_Board   = 0x80;   // the transform's bounds check reads +0xB8 / +0xBC of it
constexpr uintptr_t kBoard_W      = 0xB8;
constexpr uintptr_t kBoard_H      = 0xBC;
constexpr uintptr_t kChar_Def     = 0x240;  // GonObject* of the unit's definition (LOADCHAR 0x1400F79C6 stores the node it looked the name up to)
constexpr uintptr_t kGon_Name     = 0x88;   // std::string, the node's name = the definition name the spawn functions take (read live by mgmp_hooks' boss
                                            // predicate and mgmp_ability, so this offset is measured, not guessed)
constexpr uintptr_t kChar_Type    = 0x248;  // std::string, the DISPLAY name's text key ('ENEMY_LEAPER_NAME') -- NOT a definition name: handed to the
                                            // transform on 2026-10-03 it made the game stop with "No Character Named ENEMY_PINKY_NAME" (a fatal dialog, no SEH)
constexpr uintptr_t kChar_Faction = 0x350;  // 2 = enemies (LOADCHAR)
constexpr uintptr_t kChar_Facing  = 0x388;
constexpr uintptr_t kChar_DeadVec = 0x4D8;  // the killers list DeleteObject empties ("killed by: none")
constexpr uintptr_t kChar_NoLoot  = 0x3A9;  // DeleteObject sets it before the death
constexpr uintptr_t kChar_HP      = 0x4B0;
constexpr uintptr_t kChar_MaxHP   = 0x4BC;
constexpr uintptr_t kChar_Dead    = 0x4C2;
constexpr uintptr_t kChar_InitA   = 0x954;  // the turn-order sort's primary key
constexpr uintptr_t kChar_InitBase = 0x5DC; // the initiative base InitA is derived from
constexpr uintptr_t kChar_Key     = 0x958;  // the creation key: one value per unit for its life
constexpr uintptr_t kChar_Champion = 0xCDE; // the transform appends "_Champion" to the new name for such a unit -- a definition that may not exist (fatal)
constexpr uintptr_t kChar_RemArg  = 0xED6;  // both game callers pass Remove's bool as (this byte == 0)
constexpr uintptr_t kComp_Flags   = 0x0E;   // +0xE enabled, +0xF destroyed, +0x10 alive, +0x11 started
constexpr uintptr_t kTObj_Tile    = 0x48;
constexpr uintptr_t kTObj_Removed = 0x60;
constexpr uintptr_t kTObj_Owner   = 0x98;
constexpr int32_t   kFactionEnemy = 2;
constexpr uint32_t  kMaxList      = 256;

// The game's std::string (MSVC): 16 bytes of buffer or a heap pointer, size, capacity. A by-value argument is destroyed by
// the callee, so a heap buffer must come from the GAME's allocator (the same arrangement as mgmp_unlocks' make_key).
struct GameStr {
    union { char buf[16]; char* ptr; };
    uint64_t size;
    uint64_t cap;
};
using fn_alloc = void* (__fastcall*)(size_t);

using fn_spawn     = void* (__fastcall*)(void* unused, GameStr* name, void* world);
using fn_transform = void* (__fastcall*)(void* old, GameStr* name, bool take_slot, bool keep_hp, bool end_turn);
using fn_remove    = void  (__fastcall*)(void* tobj, bool move_sprites);
using fn_start     = void  (__fastcall*)(void* entity);
using fn_order_add = void  (__fastcall*)(void* tc, void* chr, int spawn_in, void* extra);
using fn_char_tc   = void* (__fastcall*)(void* chr);
using fn_move      = void  (__fastcall*)(void* tobj, uint64_t tile, bool, bool);
using fn_face      = void  (__fastcall*)(void* chr, uint64_t dir, bool, bool);
using fn_recompute = void  (__fastcall*)(void* chr, void* ability, bool);
using fn_die       = void  (__fastcall*)(void* chr, bool, const void* list, bool);
using fn_pop       = void  (__fastcall*)(void* chr, bool);

volatile LONG g_pending = 0;
char g_last[256] = "nothing run yet";

// Units an operation touched, followed for kFollowTurns boundaries afterwards.
struct Watch { void* chr; uint32_t key; char what[48]; int left; };
constexpr int kFollowTurns = 4;
Watch g_watch[8] = {};

// The game does not return when a definition is missing -- Character::init shows "No Character Named <name>" and the process ends,
// past any SEH -- so a name is handed over only when it was read off a live unit's own definition node and looks like one.
bool usable_def(const char* d) {
    const size_t n = strlen(d);
    if (n == 0 || n >= 63) return false;
    if (n > 5 && strcmp(d + n - 5, "_NAME") == 0) return false;
    for (size_t i = 0; i < n; ++i) if ((unsigned char)d[i] <= ' ' || (unsigned char)d[i] >= 0x7F) return false;
    return true;
}

uint64_t pack(int32_t x, int32_t y) { return ((uint64_t)(uint32_t)y << 32) | (uint32_t)x; }

bool make_str(GameStr& s, const char* text) {
    const size_t n = strlen(text);
    memset(&s, 0, sizeof(s));
    if (n <= 15) { memcpy(s.buf, text, n + 1); s.size = n; s.cap = 15; return true; }
    auto alloc = (fn_alloc)addr_of_call(C_GameStrAlloc);
    if (!alloc) return false;
    size_t cap = n | 15;
    if (cap < 22) cap = 22;
    void* p = nullptr;
    __try { p = alloc(cap + 1); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!p) return false;
    memcpy(p, text, n + 1);
    s.ptr = (char*)p; s.size = n; s.cap = cap;
    return true;
}

// The live battle list: count, and up to `cap` entries.
uint32_t read_list(void* tc, void** out, uint32_t cap) {
    const void *scene = nullptr, *sub = nullptr, *holder = nullptr, *list = nullptr;
    if (!tc || !mem_read((const uint8_t*)tc + kTC_Scene, &scene, 8) || !scene) return 0;
    if (!mem_read((const uint8_t*)scene + kScene_Sub, &sub, 8) || !sub) return 0;
    if (!mem_read((const uint8_t*)sub + kSub_Holder, &holder, 8) || !holder) return 0;
    if (!mem_read((const uint8_t*)holder + kHolder_List, &list, 8) || !list) return 0;
    uint32_t n = 0; void* data = nullptr;
    if (!mem_read((const uint8_t*)list + kList_Count, &n, 4) || !mem_read((const uint8_t*)list + kList_Data, &data, 8) || !data) return 0;
    if (n > cap) n = cap;
    return mem_read(data, out, (size_t)n * 8) ? n : 0;
}

bool in_list(void* const* list, uint32_t n, const void* chr) {
    for (uint32_t i = 0; i < n; ++i) if (list[i] == chr) return true;
    return false;
}

struct Unit {
    void* chr; void* tobj; int32_t x, y, hp, maxhp, faction; uint8_t dead, removed; uint32_t key; char type[64]; char def[64];
};

// Plain reads only. False when the unit is not a linked, on-board character.
bool read_unit(void* chr, Unit& u) {
    memset(&u, 0, sizeof(u));
    u.chr = chr;
    if (!chr || !mem_read((const uint8_t*)chr + kChar_TObj, &u.tobj, 8) || !u.tobj) return false;
    const void* owner = nullptr;
    if (!mem_read((const uint8_t*)u.tobj + kTObj_Owner, &owner, 8) || owner != chr) return false;
    int32_t t[2] = {};
    mem_read((const uint8_t*)u.tobj + kTObj_Tile, t, 8); u.x = t[0]; u.y = t[1];
    mem_read((const uint8_t*)u.tobj + kTObj_Removed, &u.removed, 1);
    mem_read((const uint8_t*)chr + kChar_HP, &u.hp, 4);
    mem_read((const uint8_t*)chr + kChar_MaxHP, &u.maxhp, 4);
    mem_read((const uint8_t*)chr + kChar_Faction, &u.faction, 4);
    mem_read((const uint8_t*)chr + kChar_Dead, &u.dead, 1);
    mem_read((const uint8_t*)chr + kChar_Key, &u.key, 4);
    if (!mem_read_std_string((const uint8_t*)chr + kChar_Type, u.type, sizeof(u.type))) strcpy_s(u.type, "?");
    const void* gon = nullptr;
    if (!mem_read((const uint8_t*)chr + kChar_Def, &gon, 8) || !gon || !mem_read_std_string((const uint8_t*)gon + kGon_Name, u.def, sizeof(u.def))) u.def[0] = 0;
    return true;
}

bool board_size(void* chr, int32_t& w, int32_t& h) {
    const void* board = nullptr;
    return mem_read((const uint8_t*)chr + kChar_Board, &board, 8) && board &&
           mem_read((const uint8_t*)board + kBoard_W, &w, 4) && mem_read((const uint8_t*)board + kBoard_H, &h, 4) && w > 0 && h > 0;
}

// An enemy the test may operate on: alive, on the board, not removed, not the one whose turn it is.
bool eligible(const Unit& u, const void* actor, int32_t w, int32_t h) {
    return u.faction == kFactionEnemy && !u.dead && !u.removed && u.hp > 0 && u.chr != actor && usable_def(u.def) &&
           u.x >= 0 && u.y >= 0 && u.x < w && u.y < h;
}

void watch(void* chr, const char* what) {
    for (Watch& w : g_watch) if (!w.chr || w.left <= 0) {
        w.chr = chr; w.left = kFollowTurns;
        mem_read((const uint8_t*)chr + kChar_Key, &w.key, 4);
        strncpy_s(w.what, what, _TRUNCATE);
        return;
    }
}

void result(LogLevel lvl, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(g_last, sizeof(g_last), _TRUNCATE, fmt, ap);
    va_end(ap);
    log_line_lvl(lvl, "SPAWNTEST", "%s", g_last);
}

// --- the calls, each under SEH: POD only in these frames ---------------------------------------------------------------

bool call_transform(void* old, GameStr* name, void*& made) {
    auto f = (fn_transform)addr_of_call(C_TransformUnit);
    if (!f) return false;
    __try { made = f(old, name, true, false, false); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool call_spawn(GameStr* name, void* world, void*& made) {
    auto f = (fn_spawn)addr_of_call(C_SpawnCharacter);
    if (!f) return false;
    __try { made = f(nullptr, name, world); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// After a spawn: what the summon paths do with a new unit, in their order (sub_140805020, LeaveBehindOnceEachMove).
int call_place(void* chr, void* tc, int32_t x, int32_t y, int32_t fx, int32_t fy, const int32_t* keys = nullptr) {
    auto move = (fn_move)addr_of_call(C_TacticsMove);
    auto face = (fn_face)addr_of_call(C_CharacterFace);
    auto rec  = (fn_recompute)addr_of_call(C_RecomputeStats);
    auto st   = (fn_start)addr_of_call(C_EntityStart);
    auto add  = (fn_order_add)addr_of_call(C_TurnOrderAdd);
    if (!move || !face || !rec || !st || !add) return -1;
    void* tobj = nullptr; void* ent = nullptr;
    mem_read((const uint8_t*)chr + kChar_TObj, &tobj, 8);
    mem_read((const uint8_t*)chr + kChar_Entity, &ent, 8);
    if (!tobj || !ent) return -2;
    int step = 0;
    __try {
        step = 1; move(tobj, pack(x, y), true, false);
        step = 2; if (fx || fy) face(chr, pack(fx, fy), true, false);
        step = 3; rec(chr, nullptr, true);
        // The turn order is a list SORTED on the unit's two initiative keys (TurnControl::SpawnIn 3 walks it and inserts before the first node that sorts below the new one), so a unit that stands
        // in for one the other peer has must carry THAT peer's keys when it is added -- written afterwards they change nothing about where it already went (2026-10-03: a unit made from the host's
        // board acted at turn 4 here and at turn 8 on the host: every later actor was one off, a halt at turn 6).
        if (keys) {
            mem_write((uint8_t*)chr + kChar_InitBase, &keys[2], 4);
            mem_write((uint8_t*)chr + kChar_InitA, &keys[0], 4);
            mem_write((uint8_t*)chr + kChar_Key, &keys[1], 4);
        }
        step = 4; st(ent);
        step = 5; add(tc, chr, 3, nullptr);
        step = 6;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 100 + step; }
    return 0;
}

int call_remove(void* chr, bool game_way) {
    auto rem = (fn_remove)addr_of_call(C_TObjRemove);
    auto die = (fn_die)addr_of_call(C_CharDie);
    auto pop = (fn_pop)addr_of_call(C_CorpsePop);
    if (!rem || (game_way && (!die || !pop))) return -1;
    void* tobj = nullptr; uint8_t arg = 0, dead = 0;
    mem_read((const uint8_t*)chr + kChar_TObj, &tobj, 8);
    mem_read((const uint8_t*)chr + kChar_RemArg, &arg, 1);
    mem_read((const uint8_t*)chr + kChar_Dead, &dead, 1);
    if (!tobj) return -2;
    int step = 0;
    __try {
        if (game_way) {
            // DeleteObject (sub_1405D9170) exactly: the killers list emptied, +0x3A9 set, Die unless dead, OnCorpsePop, Remove.
            const uint64_t z = 0; const uint8_t one = 1;
            mem_write((uint8_t*)chr + kChar_DeadVec, &z, 8);
            mem_write((uint8_t*)chr + kChar_DeadVec + 8, &z, 8);
            mem_write((uint8_t*)chr + kChar_NoLoot, &one, 1);
            step = 1; if (!dead) die(chr, true, nullptr, false);
            step = 2; pop(chr, true);
        }
        step = 3; rem(tobj, arg == 0);
        step = 4;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 100 + step; }
    return 0;
}

// --- the operations -------------------------------------------------------------------------------------------------------

// --- the layer 2 experiment: corrupt one of this peer's own player cats --------------------------------------------------------------------
constexpr uintptr_t kChar_Stats = 0x5BC, kChar_StatBonus = 0x5E8;
void read_stats(const void* chr, int32_t st[7], int32_t& bonus, int32_t& maxhp, int32_t& hp) {
    for (int k = 0; k < 7; ++k) { st[k] = 0; mem_read((const uint8_t*)chr + kChar_Stats + k * 4, &st[k], 4); }
    bonus = maxhp = hp = 0;
    mem_read((const uint8_t*)chr + kChar_StatBonus, &bonus, 4); mem_read((const uint8_t*)chr + kChar_MaxHP, &maxhp, 4); mem_read((const uint8_t*)chr + kChar_HP, &hp, 4);
}
void corrupt_stats(void** list, uint32_t n) {
    const void* chr = lockstep_dev_player_cat();
    if (!chr) {          // no session roster (single player): the first player's cat of the live list
        for (uint32_t i = 0; i < n && !chr; ++i) {
            const void* brain = nullptr; char cls[96] = {};
            if (list[i] && mem_read((const uint8_t*)list[i] + kChar_Brain, &brain, 8) && brain && rtti_class_name(brain, cls, sizeof(cls)) && strstr(cls, tune::kReplayBrains)) chr = list[i];
        }
    }
    if (!chr) { result(LogLevel::Warn, "corrupt stats: no player's cat found"); return; }
    int32_t s0[7], b0, m0, h0;
    read_stats(chr, s0, b0, m0, h0);
    log_line_lvl(LogLevel::Warn, "STATS", "EXPERIMENT corrupt: player cat %p, stats [%d %d %d %d %d %d %d] bonus %d max hp %d hp %d", chr, s0[0], s0[1], s0[2], s0[3], s0[4], s0[5], s0[6], b0, m0, h0);
    int32_t s1[7];
    for (int k = 0; k < 7; ++k) { s1[k] = s0[k] + 3; mem_write((uint8_t*)chr + kChar_Stats + k * 4, &s1[k], 4); }
    const int32_t m1 = m0 + 12;
    mem_write((uint8_t*)chr + kChar_MaxHP, &m1, 4);
    log_line_lvl(LogLevel::Warn, "STATS", "EXPERIMENT corrupt: stats now [%d %d %d %d %d %d %d] max hp %d (each stat +3, max hp +12)", s1[0], s1[1], s1[2], s1[3], s1[4], s1[5], s1[6], m1);
    // PROBE 1, the question that decides the rest: does the game's own recompute re-derive the stats from something else and undo the change?
    auto rc = (fn_recompute)addr_of_call(C_RecomputeStats);
    bool ran = false;
    if (rc) { __try { rc((void*)chr, nullptr, true); ran = true; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    int32_t s2[7], b2, m2, h2;
    read_stats(chr, s2, b2, m2, h2);
    const bool restored = !memcmp(s2, s0, sizeof(s0)) && m2 == m0, kept = !memcmp(s2, s1, sizeof(s1)) && m2 == m1;
    log_line_lvl(LogLevel::Warn, "STATS", "EXPERIMENT recompute probe after the corruption (%s): stats [%d %d %d %d %d %d %d] bonus %d max hp %d hp %d -- %s", ran ? "ran" : "NOT run",
                 s2[0], s2[1], s2[2], s2[3], s2[4], s2[5], s2[6], b2, m2, h2,
                 !ran ? "no result" : restored ? "the recompute RESTORED the original values (it re-derives them: a write is not needed, and would be undone)" : kept ? "the recompute KEPT the corrupted values (they are the inputs)" : "the recompute changed them to something else");
    result(LogLevel::Warn, "corrupt stats: %p stats +3, max hp +12; the game's recompute %s", chr,
           !ran ? "was not run" : restored ? "RESTORED the original (see the STATS log lines)" : kept ? "KEPT the corrupted values" : "gave something else");
    if (lockstep_active() || net_peer_count() >= 2) lockstep_dev_arm_stat_repair();       // the board of this boundary now writes the host's stats back, and the cat is watched
    watch(const_cast<void*>(chr), "stats corrupted");
}

void run(SpawnTestOp op, void* tc) {
    static void* list[kMaxList];
    const uint32_t n = read_list(tc, list, kMaxList);
    if (!n) { result(LogLevel::Warn, "%s: the battle list could not be read -- not in a battle?", spawntest_op_name(op)); return; }
    if (op == SpawnTestOp::CorruptStats) { corrupt_stats(list, n); return; }
    void* actor = nullptr;
    mem_read((const uint8_t*)tc + kTC_Actor, &actor, 8);

    static Unit units[kMaxList];
    uint32_t nu = 0, ne = 0;
    int32_t w = 0, h = 0;
    int first = -1;
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_unit(list[i], units[nu])) continue;
        if (!w && !board_size(list[i], w, h)) continue;
        if (eligible(units[nu], actor, w, h)) { ++ne; if (first < 0) first = (int)nu; }
        ++nu;
    }
    log_line("SPAWNTEST", "%s: %u in the battle list, %u readable, %u eligible enemies (alive, on the %dx%d board, not the actor %p)",
             spawntest_op_name(op), n, nu, ne, w, h, actor);
    if (first < 0) { result(LogLevel::Warn, "%s: no eligible enemy -- nothing done", spawntest_op_name(op)); return; }
    {
        // The enemy nearest the middle of the board, so it is on screen and easy to find (the first in the list was on the edge row).
        // A transform also skips champions: it would ask for "<kind>_Champion", which need not exist.
        first = -1;
        int32_t best = 0x7FFFFFFF;
        for (uint32_t i = 0; i < nu; ++i) {
            if (!eligible(units[i], actor, w, h)) continue;
            if (op == SpawnTestOp::Transform) {
                uint8_t champ = 1;
                if (!mem_read((const uint8_t*)units[i].chr + kChar_Champion, &champ, 1) || champ) continue;
            }
            const int32_t dx = units[i].x * 2 - (w - 1), dy = units[i].y * 2 - (h - 1);
            if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; first = (int)i; }
        }
        if (first < 0) { result(LogLevel::Warn, "%s: no suitable enemy (all champions?) -- nothing done", spawntest_op_name(op)); return; }
    }
    {
        char kinds[200]; int k = 0; kinds[0] = 0;
        for (uint32_t i = 0; i < nu && k < (int)sizeof(kinds) - 40; ++i) {
            if (!eligible(units[i], actor, w, h)) continue;
            const int r = _snprintf_s(kinds + k, sizeof(kinds) - k, _TRUNCATE, " %s", units[i].def);
            if (r < 0) break;
            k += r;
        }
        log_line("SPAWNTEST", "  eligible definitions:%s", kinds);
    }
    const Unit t = units[first];

    if (op == SpawnTestOp::Transform) {
        // Another enemy's kind, so the result is visibly different; the target's own kind when every enemy is the same one.
        const char* kind = t.def;
        for (uint32_t i = 0; i < nu; ++i)
            if (eligible(units[i], actor, w, h) && strcmp(units[i].def, t.def) != 0) { kind = units[i].def; break; }
        GameStr s;
        if (!make_str(s, kind)) { result(LogLevel::Error, "transform: the name could not be built"); return; }
        void* made = nullptr;
        log_line("SPAWNTEST", "transform: '%s' (%s) %p at (%d,%d) hp %d/%d key %08X -> '%s'", t.def, t.type, t.chr, t.x, t.y, t.hp, t.maxhp, t.key, kind);
        if (!call_transform(t.chr, &s, made)) { result(LogLevel::Error, "transform FAULTED on '%s' -> '%s' (the call raised an exception)", t.def, kind); return; }
        if (!made) { result(LogLevel::Warn, "transform returned null for '%s' -> '%s' (the game refused: off the board or removed)", t.def, kind); return; }
        Unit m; read_unit(made, m);
        uint8_t old_removed = 0xFF;
        mem_read((const uint8_t*)t.tobj + kTObj_Removed, &old_removed, 1);
        log_line("SPAWNTEST", "  the old unit's board object %p: removed=%u; the new one's %p: removed=%u", t.tobj, old_removed, m.tobj, m.removed);
        result(LogLevel::Warn, "transform OK at column %d, row %d (0-based, board %dx%d): %s hp %d/%d -> %s hp %d/%d (%p -> %p)", m.x, m.y, w, h, t.def, t.hp, t.maxhp, m.def, m.hp, m.maxhp, t.chr, made);
        lockstep_roster_replace(t.chr, made);      // inside a session the roster must follow the replacement (a no-op otherwise)
        watch(t.chr, "transformed (old)");
        watch(made, "transformed (new)");
        return;
    }

    if (op == SpawnTestOp::Spawn) {
        // A free tile next to the enemy: on the board and no unit of the list standing on it. Walls are not known here --
        // if the tile is one, the read-back below says where the unit really ended.
        static const int32_t d[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,-1},{1,-1},{-1,1} };
        int32_t x = 0, y = 0; bool found = false;
        for (int r = 1; r <= 3 && !found; ++r)
            for (int k = 0; k < 8 && !found; ++k) {
                x = t.x + d[k][0] * r; y = t.y + d[k][1] * r;
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                bool taken = false;
                for (uint32_t i = 0; i < nu && !taken; ++i) if (!units[i].removed && units[i].x == x && units[i].y == y) taken = true;
                found = !taken;
            }
        if (!found) { result(LogLevel::Warn, "spawn: no free tile within 3 of '%s' at (%d,%d)", t.def, t.x, t.y); return; }
        void* world = nullptr;
        mem_read((const uint8_t*)t.chr + kChar_World, &world, 8);
        int32_t f[2] = {};
        mem_read((const uint8_t*)t.chr + kChar_Facing, f, 8);
        GameStr s;
        if (!world || !make_str(s, t.def)) { result(LogLevel::Error, "spawn: no world or the name could not be built"); return; }
        void* made = nullptr;
        if (!call_spawn(&s, world, made)) { result(LogLevel::Error, "spawn FAULTED making '%s'", t.def); return; }
        if (!made) { result(LogLevel::Warn, "spawn returned null for '%s' (the world is being torn down?)", t.def); return; }
        auto ctc = (fn_char_tc)addr_of_call(C_CharTurnControl);
        void* tc2 = nullptr;
        __try { tc2 = ctc ? ctc(t.chr) : nullptr; } __except (EXCEPTION_EXECUTE_HANDLER) { tc2 = nullptr; }
        if (tc2 != tc) log_line_lvl(LogLevel::Warn, "SPAWNTEST", "spawn: the unit's TurnControl is %p, the hook's %p -- the hook's is used", tc2, tc);
        const int rc = call_place(made, tc, x, y, f[0], f[1]);
        Unit m; read_unit(made, m);
        uint8_t flags[4] = {};
        mem_read((const uint8_t*)made + kComp_Flags, flags, 4);
        const void* brain = nullptr;
        mem_read((const uint8_t*)made + kChar_Brain, &brain, 8);
        result(rc ? LogLevel::Error : LogLevel::Warn, "spawn %s (rc %d): '%s' %p asked for (%d,%d), stands on (%d,%d) hp %d/%d faction %d key %08X brain %p flags e%u f%u a%u s%u",
               rc ? "FAILED" : "OK", rc, m.def, made, x, y, m.x, m.y, m.hp, m.maxhp, m.faction, m.key, brain, flags[0], flags[1], flags[2], flags[3]);
        watch(made, "spawned");
        return;
    }

    const bool game_way = op == SpawnTestOp::RemoveGameWay;
    log_line("SPAWNTEST", "%s: '%s' %p at (%d,%d) hp %d/%d key %08X", spawntest_op_name(op), t.def, t.chr, t.x, t.y, t.hp, t.maxhp, t.key);
    const int rc = call_remove(t.chr, game_way);
    Unit m; read_unit(t.chr, m);
    result(rc ? LogLevel::Error : LogLevel::Warn, "%s %s (rc %d): '%s' %p now at (%d,%d) removed %u dead %u hp %d",
           spawntest_op_name(op), rc ? "FAILED" : "OK", rc, t.def, t.chr, m.x, m.y, m.removed, m.dead, m.hp);
    watch(t.chr, game_way ? "removed (game way)" : "removed (silent)");
}

// What became of the touched units: still in the list? removed, dead, where, brain, and is it the actor now.
void follow(void* tc) {
    bool any = false;
    for (Watch& w : g_watch) if (w.chr && w.left > 0) { any = true; break; }
    if (!any) return;
    static void* list[kMaxList];
    const uint32_t n = read_list(tc, list, kMaxList);
    void* actor = nullptr;
    mem_read((const uint8_t*)tc + kTC_Actor, &actor, 8);
    for (Watch& w : g_watch) {
        if (!w.chr || w.left <= 0) continue;
        --w.left;
        const bool listed = in_list(list, n, w.chr);
        uint32_t key = 0;
        mem_read((const uint8_t*)w.chr + kChar_Key, &key, 4);
        if (!listed) {
            // Not in the list: possibly freed already, so nothing but the key (a guarded read) is looked at.
            log_line("SPAWNTEST", "  follow %s %p: NOT in the battle list (%u entries); key now %08X (was %08X)%s",
                     w.what, w.chr, n, key, w.key, key != w.key ? " -- the memory has another occupant" : "");
            continue;
        }
        Unit u; const bool linked = read_unit(w.chr, u);
        uint8_t flags[4] = {};
        mem_read((const uint8_t*)w.chr + kComp_Flags, flags, 4);
        const void* brain = nullptr;
        mem_read((const uint8_t*)w.chr + kChar_Brain, &brain, 8);
        log_line("SPAWNTEST", "  follow %s %p: in the list, %s '%s' at (%d,%d) hp %d/%d dead %u removed %u brain %p flags e%u f%u a%u s%u%s",
                 w.what, w.chr, linked ? "linked" : "UNLINKED", u.def, u.x, u.y, u.hp, u.maxhp, u.dead, u.removed, brain,
                 flags[0], flags[1], flags[2], flags[3], w.chr == actor ? " -- IT IS THE ACTOR" : "");
    }
}

} // namespace

bool unit_definition_name(const void* chr, char* out, unsigned cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    const void* gon = nullptr;
    char tmp[64] = {};
    if (!chr || !mem_read((const uint8_t*)chr + kChar_Def, &gon, 8) || !gon || !mem_read_std_string((const uint8_t*)gon + kGon_Name, tmp, sizeof(tmp)) || !usable_def(tmp)) return false;
    if (strlen(tmp) >= cap) return false;
    strcpy_s(out, cap, tmp);
    return true;
}

bool unit_is_champion(const void* chr) {
    uint8_t c = 0;
    return chr && mem_read((const uint8_t*)chr + kChar_Champion, &c, 1) && c != 0;
}

void* unit_transform_to(void* old, const char* def, const char*& why) {
    why = nullptr;
    if (!old || !def || !usable_def(def)) { why = "no usable definition name"; return nullptr; }
    GameStr s;
    if (!make_str(s, def)) { why = "the name could not be built"; return nullptr; }
    void* made = nullptr;
    if (!call_transform(old, &s, made)) { why = "the transform raised an exception"; return nullptr; }
    if (!made) why = "the game refused (the unit is off the board or already removed)";
    return made;
}

bool unit_is_removed(const void* chr) {
    const void* tobj = nullptr;
    uint8_t r = 0;
    return chr && mem_read((const uint8_t*)chr + kChar_TObj, &tobj, 8) && tobj && mem_read((const uint8_t*)tobj + kTObj_Removed, &r, 1) && r != 0;
}

bool unit_is_pickup(const void* chr) {
    char t[64] = {};
    return chr && mem_read_std_string((const uint8_t*)chr + kChar_Type, t, sizeof(t)) && strncmp(t, "PICKUP_", 7) == 0;
}

bool unit_board_size(const void* chr, int32_t& w, int32_t& h) {
    w = h = 0;
    return chr && board_size(const_cast<void*>(chr), w, h);
}

void* unit_spawn_at(void* tc, const void* like, const char* def, int32_t x, int32_t y, const int32_t* keys, const char*& why) {
    why = nullptr;
    if (!tc || !like || !def || !usable_def(def)) { why = "no usable definition name or no unit to borrow the world from"; return nullptr; }
    void* world = nullptr;
    int32_t f[2] = {};
    mem_read((const uint8_t*)like + kChar_World, &world, 8);
    mem_read((const uint8_t*)like + kChar_Facing, f, 8);
    GameStr s;
    if (!world || !make_str(s, def)) { why = "no world, or the name could not be built"; return nullptr; }
    void* made = nullptr;
    if (!call_spawn(&s, world, made)) { why = "the spawn raised an exception"; return nullptr; }
    if (!made) { why = "the game returned no unit"; return nullptr; }
    const int rc = call_place(made, tc, x, y, f[0], f[1], keys);
    if (rc) { why = "placing the new unit failed"; log_line_lvl(LogLevel::Error, "SPAWNTEST", "unit_spawn_at: placing '%s' %p at (%d,%d) failed (rc %d)", def, made, x, y, rc); return nullptr; }
    return made;
}

// --- 0.1.190: revive, an ability by name, a unit from cat data ------------------------------------------------------------------------------
namespace {
constexpr uintptr_t kChar_AbilSlot0  = 0xD0;   // move, attack, bonus: three Ability*; then the spells' CustomVector {u32 cap, u32 count, Ability** data}
constexpr uintptr_t kChar_SpellCap   = 0xE8, kChar_SpellCount = 0xEC, kChar_SpellData = 0xF0;
constexpr uintptr_t kCat_SpellNames  = 0x810;  // CatData: the cat's spells, std::string each (0x20); the console's set-ability writes the first four
constexpr uint32_t  kCatSpellNames   = 4;
constexpr uintptr_t kAbil_FromItem   = 0x8A1;  // +0x8A1 weapon, +0x8A2 trinket: an ability an item gave (mgmp_ability.h)
// SpawnDatabase+0x198: the ability definitions (3076 on 2026-11-05) -- CreateAbility looks an ordinary name up here and an unknown one ends in the game's error
// report (0x1407ACEDC); the console's set-ability checks the same node first. (+0x3A8 is the four ability POOLS: a name starting "RandomFrom" draws from one.)
constexpr uintptr_t kDb_AbilityGon   = 0x198;
constexpr uintptr_t kGon_Kind        = 0xA8;   // 0 = the game's empty node: no such child
constexpr uintptr_t kTempAbil_DbLoad = 0x1C0;  // C_AddTempAbility + 0x1C0: 48 8B 1D rel32 = mov rbx, [the SpawnDatabase pointer]
constexpr uint32_t  kMaxSpells       = 64;

using fn_revive   = void  (__fastcall*)(void* chr, int32_t hp);
using fn_mkabil   = void* (__fastcall*)(void* db, void* owner, const GameStr* name, void* extra);
using fn_spawncat = void* (__fastcall*)(void* unused, GameStr* name, void* cat, void* world);
using fn_uiof     = void* (__fastcall*)(void* chr);
using fn_uibuild  = void  (__fastcall*)(void* comp, bool unused);
using fn_gonget   = const uint8_t* (__fastcall*)(const void* gon, const GameStr* key);
using fn_deldtor  = void* (__fastcall*)(void* obj, uint32_t flags);
using fn_assign   = void* (__fastcall*)(void* str, const char* p, size_t n);

void* spawn_database() {
    const uintptr_t f = addr_of_call(C_AddTempAbility);
    if (!f) return nullptr;
    uint8_t op[3] = {}; int32_t rel = 0;
    if (!mem_read((const void*)(f + kTempAbil_DbLoad), op, 3) || op[0] != 0x48 || op[1] != 0x8B || op[2] != 0x1D ||
        !mem_read((const void*)(f + kTempAbil_DbLoad + 3), &rel, 4)) return nullptr;
    void* db = nullptr;
    mem_read((const void*)(f + kTempAbil_DbLoad + 7 + (intptr_t)rel), &db, 8);
    return db;
}
// The game's ability definitions know the name (a name past 15 characters leaves its game-heap buffer behind: a few dozen bytes per repair).
bool ability_known(void* db, const GameStr& key) {
    auto get = (fn_gonget)addr_of_call(C_GonGet);
    if (!get || !db) return false;
    const uint8_t* a = nullptr;
    __try { a = get((const uint8_t*)db + kDb_AbilityGon, &key); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    int32_t ka = 0;
    return a && mem_read(a + kGon_Kind, &ka, 4) && ka != 0;
}
bool from_item(const void* ab) { uint8_t f[2] = {}; return ab && mem_read((const uint8_t*)ab + kAbil_FromItem, f, 2) && (f[0] || f[1]); }
bool destroy_ability(void* ab) {          // vtable slot 0, the deleting destructor, as the console calls it
    void* const* vt = nullptr; void* fn = nullptr;
    if (!ab || !mem_read(ab, &vt, 8) || !vt || !mem_read(vt, &fn, 8) || !fn) return false;
    __try { ((fn_deldtor)fn)(ab, 1); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void rebuild_bar(void* chr) {
    auto of = (fn_uiof)addr_of_call(C_AbilityUiOf);
    auto build = (fn_uibuild)addr_of_call(C_AbilityUiBuild);
    if (!of || !build) return;
    __try { if (void* c = of(chr)) build(c, false); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
} // namespace

bool unit_revive(void* chr, int32_t hp) {
    auto f = (fn_revive)addr_of_call(C_CharRevive);
    if (!f || !chr) return false;
    __try { f(chr, hp > 0 ? hp : 1); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    uint8_t dead = 1;
    return mem_read((const uint8_t*)chr + kChar_Dead, &dead, 1) && dead == 0;
}

bool unit_die(void* chr) {
    auto die = (fn_die)addr_of_call(C_CharDie);
    uint8_t dead = 0;
    if (!die || !chr || !mem_read((const uint8_t*)chr + kChar_Dead, &dead, 1)) return false;
    if (dead) return true;
    __try { die(chr, false, nullptr, false); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return mem_read((const uint8_t*)chr + kChar_Dead, &dead, 1) && dead != 0;
}

bool unit_set_ability(void* chr, uint8_t slot, const char* name, bool cat_too, const char*& why) {
    why = nullptr;
    auto make = (fn_mkabil)addr_of_call(C_CreateAbility);
    void* db = spawn_database();
    if (!chr || !make || !db) { why = "the game's ability factory is not resolved"; return false; }
    if (!name || !usable_def(name)) { why = "not a usable ability name"; return false; }
    if (_strnicmp(name, "RandomFrom", 10) == 0 || _stricmp(name, "none") == 0) { why = "a pool or 'none', not an ability (the factory would draw one or make nothing)"; return false; }
    GameStr key;
    if (!make_str(key, name)) { why = "the name could not be built"; return false; }
    if (!ability_known(db, key)) { why = "the game knows no ability of that name"; return false; }
    uint8_t* c = (uint8_t*)chr;
    uint8_t* where = nullptr;
    bool append = false;
    uint32_t idx = 0;
    if (slot < 3) where = c + kChar_AbilSlot0 + slot * 8;
    else {
        idx = slot - 3u;
        uint32_t cap = 0, count = 0; uint8_t* data = nullptr;
        if (!mem_read(c + kChar_SpellCap, &cap, 4) || !mem_read(c + kChar_SpellCount, &count, 4) || !mem_read(c + kChar_SpellData, &data, 8) || count > kMaxSpells || cap > 4096) {
            why = "the spell list is unreadable"; return false;
        }
        if (idx > count) { why = "a gap in the spell list (an earlier slot is missing too)"; return false; }
        if (idx == count) {
            if (!data || count >= cap) { why = "the spell list has no room for one more"; return false; }
            append = true;
        }
        where = data + idx * 8;
    }
    void* old = nullptr;
    if (!append && !mem_read(where, &old, 8)) { why = "the slot is unreadable"; return false; }
    if (old && from_item(old)) { why = "the slot holds an item's ability (the gear repair owns it)"; return false; }
    void* made = nullptr;
    __try { made = make(db, chr, &key, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { made = nullptr; }
    if (!made) { why = "the game's factory made nothing"; return false; }
    mem_write(where, &made, 8);
    if (append) { const uint32_t n = idx + 1; mem_write(c + kChar_SpellCount, &n, 4); }
    if (old && !destroy_ability(old)) log_line_lvl(LogLevel::Warn, "USYNC", "!! the old ability of slot %u (%p) faulted in its destructor -- left as it is", (unsigned)slot, old);
    if (cat_too && slot >= 3 && idx < kCatSpellNames) {
        uint8_t* cat = nullptr;
        auto assign = (fn_assign)addr_of_call(C_StrAssign);
        if (assign && mem_read(c + kChar_CatData, &cat, 8) && cat) {
            __try { assign(cat + kCat_SpellNames + idx * 0x20, name, strlen(name)); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    rebuild_bar(chr);
    return true;
}

bool unit_trim_spells(void* chr, uint32_t keep, const char*& why) {
    why = nullptr;
    uint8_t* c = (uint8_t*)chr;
    uint32_t count = 0; uint8_t* data = nullptr;
    if (!chr || !mem_read(c + kChar_SpellCount, &count, 4) || !mem_read(c + kChar_SpellData, &data, 8) || !data || count > kMaxSpells) { why = "the spell list is unreadable"; return false; }
    while (count > keep) {
        void* ab = nullptr;
        if (!mem_read(data + (count - 1) * 8, &ab, 8)) { why = "a spell slot is unreadable"; return false; }
        if (from_item(ab)) { why = "the last spell is an item's ability (the gear repair owns it)"; return false; }
        void* const none = nullptr;
        mem_write(data + (count - 1) * 8, &none, 8);
        --count;
        mem_write(c + kChar_SpellCount, &count, 4);
        if (ab && !destroy_ability(ab)) log_line_lvl(LogLevel::Warn, "USYNC", "!! a trimmed spell (%p) faulted in its destructor -- left as it is", ab);
    }
    rebuild_bar(chr);
    return true;
}

void* unit_spawn_catdata_at(void* tc, const void* like, const char* def, void* cat, int32_t x, int32_t y, const int32_t* keys, const char*& why) {
    why = nullptr;
    auto spawn = (fn_spawncat)addr_of_call(C_SpawnWithCatData);
    if (!spawn) { why = "the game's cat-data spawn is not resolved"; return nullptr; }
    if (!tc || !like || !def || !cat || !usable_def(def)) { why = "no usable definition name, no cat data or no unit to borrow the world from"; return nullptr; }
    void* world = nullptr;
    int32_t f[2] = {};
    mem_read((const uint8_t*)like + kChar_World, &world, 8);
    mem_read((const uint8_t*)like + kChar_Facing, f, 8);
    GameStr s;
    if (!world || !make_str(s, def)) { why = "no world, or the name could not be built"; return nullptr; }
    void* made = nullptr;
    __try { made = spawn(nullptr, &s, cat, world); } __except (EXCEPTION_EXECUTE_HANDLER) { why = "the spawn raised an exception"; return nullptr; }
    if (!made) { why = "the game returned no unit"; return nullptr; }
    const int rc = call_place(made, tc, x, y, f[0], f[1], keys);
    if (rc) { why = "placing the new unit failed"; log_line_lvl(LogLevel::Error, "SPAWNTEST", "unit_spawn_catdata_at: placing '%s' %p at (%d,%d) failed (rc %d)", def, made, x, y, rc); return nullptr; }
    return made;
}

bool unit_remove_silent(void* chr) { return chr && call_remove(chr, false) == 0; }
bool unit_remove_game_way(void* chr) { return chr && call_remove(chr, true) == 0; }

const char* spawntest_op_name(SpawnTestOp op) {
    switch (op) {
        case SpawnTestOp::Transform:     return "transform";
        case SpawnTestOp::Spawn:         return "spawn";
        case SpawnTestOp::RemoveSilent:  return "remove (silent)";
        case SpawnTestOp::RemoveGameWay: return "remove (game way)";
        case SpawnTestOp::CorruptStats:  return "corrupt my cat's stats";
        default:                         return "none";
    }
}

void spawntest_request(SpawnTestOp op) {
    if (!config().dev_tools) return;
    InterlockedExchange(&g_pending, (LONG)op);
    log_line_lvl(LogLevel::Warn, "SPAWNTEST", "requested: %s -- runs at the next turn boundary", spawntest_op_name(op));
}

SpawnTestOp spawntest_pending() { return (SpawnTestOp)g_pending; }
const char* spawntest_last_result() { return g_last; }

void spawntest_turn_boundary(void* turn_control) {
    if (!config().dev_tools || !turn_control) return;
    follow(turn_control);
    const SpawnTestOp op = (SpawnTestOp)InterlockedExchange(&g_pending, 0);
    if (op == SpawnTestOp::None) return;
    // In a session this peer's board would change and the other's not. Single player: anything. A CLIENT may transform an enemy, on purpose, to make a kind difference the host's board
    // (board_apply) then has to repair at this same turn boundary -- the end-to-end test of that repair. Everything else is refused: the sync cannot yet repair a missing or extra unit.
    if (lockstep_active() || net_peer_count() >= 2) {
        // ANY operation, on either peer (2026-10-03): on a CLIENT it is a deliberate desync the host's board of this same boundary must repair; on the HOST it changes the authoritative
        // board, and the client has to follow it.
        log_line_lvl(LogLevel::Warn, "SPAWNTEST", net_role() == NetRole::Client
            ? "DELIBERATE DESYNC on a client: %s -- the host's board of this boundary should put it back"
            : "ON THE HOST: %s -- the host's board of this boundary is the truth now, the clients should follow it", spawntest_op_name(op));
    }
    run(op, turn_control);
}

} // namespace mgmp
