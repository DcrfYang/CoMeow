// mgmp_saveplace.cpp -- see mgmp_saveplace.h.
#include "mgmp_saveplace.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "mgmp_checkpoint_io.h"
#include "mgmp_i18n.h"
#include "mgmp_log.h"

namespace mgmp {
namespace {

// The node keys of the maps' gon files (data/maps/*.gon, standard_nodes.gon included by every map) with the label each one gets. A key not here is
// looked up again without a "mw_" prefix (the Throbbing Domain's copies) and without trailing digits (exit0, furniturebox2, mw_battle1).
struct Kind { const char* key; Tx label; };
const Kind kKinds[] = {
    { "start",            Tx::PLACE_START },         // type enter: the map's starting point
    { "battle",           Tx::NODE_BATTLE },
    { "battle_testskip",  Tx::NODE_BATTLE },
    { "hard",             Tx::NODE_HARD },
    { "hard_initial",     Tx::NODE_HARD },           // alley / sewers: the first hard battle of the hard path
    { "hard_testskip",    Tx::NODE_HARD },
    { "c4hard",           Tx::NODE_HARD },           // the Infinite's hard battles (boss music)
    { "miniboss",         Tx::NODE_MINIBOSS },
    { "miniboss_event",   Tx::NODE_MUTANT },         // the Lab: Stacy's mutant event (GenFlag_Boss_Stacy), an empty node otherwise
    { "boss",             Tx::NODE_BOSS },
    { "event",            Tx::NODE_EVENT },
    { "optional_event",   Tx::NODE_OPT_EVENT },
    { "quest_event",      Tx::NODE_STORY },          // the maps' story node (the Lab's broken time machine, the Caves', the Core's ...)
    { "time_machine",     Tx::NODE_TIME_MACHINE },   // Ice Age, Jurassic, Future, The End
    { "weather_event",    Tx::NODE_WEATHER },        // the Crater
    { "altar",            Tx::NODE_ALTAR },          // the Throbbing Domain (mw_altar)
    { "shop",             Tx::NODE_SHOP },
    { "shop_water",       Tx::NODE_WATER_SHOP },     // the Desert
    { "shop_cheapwater",  Tx::NODE_CHEAP_WATER },    // the Desert
    { "treasure",         Tx::NODE_TREASURE },
    { "treasure_hard",    Tx::NODE_RARE_TREASURE },
    { "foodbox",          Tx::NODE_FOODBOX },
    { "furniturebox",     Tx::NODE_FURNITURE },      // furniturebox / 2 / 3
    { "abilityneedle",    Tx::NODE_NEEDLE },         // the Lab: the injection (Treasure_AbilityNeedle)
    { "largemoneybag",    Tx::NODE_MONEYBAG },
    { "smallmoneybag",    Tx::NODE_MONEYBAG },
    { "largemeteor",      Tx::NODE_METEOR },
    { "smallmeteor",      Tx::NODE_METEOR },
    { "treasure_radiated",Tx::NODE_RADIATED },       // The End
    { "ancestor_bones",   Tx::NODE_ANCESTOR },       // the Jurassic
    { "glowing_bones",    Tx::NODE_GLOWBONE },       // the Ice Age
    { "home",             Tx::NODE_HOME },
    { "earlyhome",        Tx::NODE_HOME },           // the Throbbing Domain (mw_earlyhome)
    { "exit",             Tx::NODE_EXIT },           // exit0 / exit1 / exit_desert / exit_lab
    { "exit_desert",      Tx::NODE_EXIT },
    { "exit_lab",         Tx::NODE_EXIT },
    { "npc",              Tx::NODE_NPC },
    { "node",             Tx::NODE_EMPTY },          // type empty
};

const Kind* find_kind(const char* key) {
    for (const Kind& k : kKinds) if (strcmp(k.key, key) == 0) return &k;
    return nullptr;
}

const Kind* lookup(const char* kind) {
    if (!kind || !kind[0]) return nullptr;
    char k[40]; strncpy_s(k, sizeof(k), kind, _TRUNCATE);
    if (const Kind* r = find_kind(k)) return r;
    const char* base = strncmp(k, "mw_", 3) == 0 ? k + 3 : k;
    if (const Kind* r = find_kind(base)) return r;
    char t[40]; strncpy_s(t, sizeof(t), base, _TRUNCATE);
    size_t n = strlen(t);
    while (n && t[n - 1] >= '0' && t[n - 1] <= '9') t[--n] = 0;
    return n ? find_kind(t) : nullptr;
}

struct Map { const char* file; Tx name; };
const Map kMaps[] = {
    { "tutorial", Tx::AREA_TUTORIAL }, { "alley", Tx::AREA_ALLEY }, { "sewers", Tx::AREA_SEWERS }, { "junkyard", Tx::AREA_JUNKYARD },
    { "caves", Tx::AREA_CAVES }, { "boneyard", Tx::AREA_BONEYARD }, { "meatworld", Tx::AREA_MEATWORLD }, { "desert", Tx::AREA_DESERT },
    { "bunker", Tx::AREA_BUNKER }, { "crater", Tx::AREA_CRATER }, { "core", Tx::AREA_CORE }, { "moon", Tx::AREA_MOON },
    { "dimensionx", Tx::AREA_DIMENSIONX }, { "lab", Tx::AREA_LAB }, { "iceage", Tx::AREA_ICEAGE }, { "future", Tx::AREA_FUTURE },
    { "jurassic", Tx::AREA_JURASSIC }, { "theend", Tx::AREA_THEEND }, { "endoftime", Tx::AREA_ENDOFTIME },
};
const char* map_name(const char* file) {
    for (const Map& m : kMaps) if (strcmp(m.file, file) == 0) return tr(m.name);
    return file;
}

// A bounds-checked reader over the chapter_map row.
struct Reader {
    const uint8_t* p; size_t n, o = 0; bool ok = true;
    Reader(const uint8_t* d, size_t len) : p(d), n(len) {}
    bool skip(size_t k) { if (!ok || n - o < k) { ok = false; return false; } o += k; return true; }
    uint32_t u32() { uint32_t v = 0; if (n - o >= 4 && ok) { memcpy(&v, p + o, 4); o += 4; } else ok = false; return v; }
    uint64_t u64() { uint64_t v = 0; if (n - o >= 8 && ok) { memcpy(&v, p + o, 8); o += 8; } else ok = false; return v; }
    uint8_t  u8()  { uint8_t v = 0; if (n - o >= 1 && ok) v = p[o++]; else ok = false; return v; }
    // a length-prefixed string, kept up to cap-1 bytes; one longer than 4 KiB is not a name
    void str(char* out, size_t cap) {
        const uint64_t len = u64();
        if (!ok || len > 4096 || n - o < len) { ok = false; if (cap) out[0] = 0; return; }
        const size_t c = (size_t)len < cap - 1 ? (size_t)len : cap - 1;
        memcpy(out, p + o, c); out[c] = 0; o += (size_t)len;
    }
};

void set_why(SavePlaceInfo& r, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(r.why, sizeof(r.why), fmt, ap); va_end(ap);
    r.place = SavePlace::Unknown;
}

void strip_gon(char* s) {
    const size_t n = strlen(s);
    if (n > 4 && _stricmp(s + n - 4, ".gon") == 0) s[n - 4] = 0;
}

struct CacheEntry { uint64_t size = 0; FILETIME wrote{}; SavePlaceInfo info; };
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<std::wstring, CacheEntry> g_cache;

bool file_stamp(const std::wstring& path, uint64_t& size, FILETIME& wrote) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow; wrote = a.ftLastWriteTime;
    return true;
}

bool read_int(const std::wstring& db, const char* key, int64_t& out) {
    bool have = false; std::string text;
    if (!checkpoint_io::read_property(db, key, have, text)) return false;
    out = have ? (int64_t)_strtoi64(text.c_str(), nullptr, 10) : -1;
    return true;
}

} // namespace

const char* saveplace_node_label(const char* kind) {
    const Kind* k = lookup(kind);
    return k ? tr(k->label) : nullptr;
}

SavePlaceInfo saveplace_decode(bool readable, int64_t on_adventure, int64_t adventure_started, const std::vector<uint8_t>* chapter_map) {
    SavePlaceInfo r;
    if (!readable) { set_why(r, "the save could not be read"); return r; }
    if (on_adventure <= 0) { r.place = SavePlace::House; return r; }               // 0, or no such property: a save that never left the warehouse
    if (on_adventure != 1) { set_why(r, "on_adventure is %lld", (long long)on_adventure); return r; }
    const bool have_map = chapter_map && !chapter_map->empty();
    char file[64] = {};
    if (have_map) {
        Reader m(chapter_map->data(), chapter_map->size());
        m.skip(32); m.str(file, sizeof(file));
        if (m.ok) { strip_gon(file); strncpy_s(r.map, sizeof(r.map), file, _TRUNCATE); }
    }
    if (adventure_started <= 0) { r.place = have_map ? SavePlace::Chapter : SavePlace::Gear; return r; }
    if (adventure_started != 1) { set_why(r, "adventure_started is %lld", (long long)adventure_started); return r; }
    if (!have_map) { set_why(r, "on the map (adventure_started 1) but the save has no chapter_map"); return r; }

    Reader m(chapter_map->data(), chapter_map->size());
    m.skip(32); m.str(file, sizeof(file));
    const uint32_t version = m.u32(); m.u32(); m.u8();
    char cur[64] = {}; m.str(cur, sizeof(cur));
    const uint32_t cur_col = m.u32();
    const uint64_t count = m.u64();
    if (!m.ok) { set_why(r, "chapter_map header unreadable (%u bytes)", (unsigned)chapter_map->size()); return r; }
    if (version != 10) log_line_lvl(LogLevel::Warn, "SAVEPLACE", "!! chapter_map version %u (10 expected) -- read as version 10", version);
    if (count > 512) { set_why(r, "chapter_map holds %llu nodes", (unsigned long long)count); return r; }
    bool found = false;
    for (uint64_t i = 0; i < count && m.ok; ++i) {
        char name[64] = {}, level[64] = {};
        m.str(name, sizeof(name));
        const uint32_t col = m.u32();
        const uint8_t finished = m.u8();
        m.skip(32);
        m.str(level, sizeof(level));
        if (m.ok && !found && col == cur_col && strcmp(name, cur) == 0) {
            found = true; r.done = finished != 0;
            strncpy_s(r.level, sizeof(r.level), level, _TRUNCATE);
        }
    }
    if (!m.ok) { set_why(r, "chapter_map node list unreadable (%u bytes, %llu nodes)", (unsigned)chapter_map->size(), (unsigned long long)count); return r; }
    if (!found) { set_why(r, "the current node '%s' (column %u) is not in the map's %llu nodes", cur, cur_col, (unsigned long long)count); return r; }
    strncpy_s(r.kind, sizeof(r.kind), cur, _TRUNCATE);
    const Kind* k = lookup(cur);
    if (!k) { set_why(r, "node kind '%s' (level '%s') on map '%s' has no label", cur, r.level, r.map); strncpy_s(r.kind, sizeof(r.kind), cur, _TRUNCATE); return r; }
    r.place = SavePlace::Map;
    r.start = k->label == Tx::PLACE_START;
    return r;
}

SavePlaceInfo saveplace_read(const std::wstring& db, bool cached) {
    uint64_t size = 0; FILETIME wrote{};
    const bool stamped = file_stamp(db, size, wrote);
    if (cached && stamped) {
        AcquireSRWLockShared(&g_lock);
        auto it = g_cache.find(db);
        const bool hit = it != g_cache.end() && it->second.size == size && CompareFileTime(&it->second.wrote, &wrote) == 0;
        SavePlaceInfo r; if (hit) r = it->second.info;
        ReleaseSRWLockShared(&g_lock);
        if (hit) return r;
    }
    int64_t oa = -1, as = -1;
    bool readable = stamped && read_int(db, "on_adventure", oa) && read_int(db, "adventure_started", as);
    std::vector<uint8_t> row; int have = 0;
    if (readable && oa == 1 && !checkpoint_io::read_file_row(db, "chapter_map", have, row)) readable = false;
    if (!have) row.clear();
    SavePlaceInfo r = saveplace_decode(readable, oa, as, &row);
    if (r.place == SavePlace::Unknown)
        log_line_lvl(LogLevel::Warn, "SAVEPLACE", "!! where the save '%ls' stands is unknown: %s (on_adventure %lld, adventure_started %lld, chapter_map %u bytes) -- shown as unknown",
                     db.c_str(), r.why, (long long)oa, (long long)as, (unsigned)row.size());
    if (cached && stamped) {
        AcquireSRWLockExclusive(&g_lock);
        if (g_cache.size() > 512) g_cache.clear();
        g_cache[db] = CacheEntry{ size, wrote, r };
        ReleaseSRWLockExclusive(&g_lock);
    }
    return r;
}

void saveplace_text(const SavePlaceInfo& p, char* out, size_t cap) {
    if (!out || !cap) return;
    switch (p.place) {
        case SavePlace::House:   _snprintf_s(out, cap, _TRUNCATE, "%s", tr(Tx::PAGE_HOUSE)); return;
        case SavePlace::Collar:  _snprintf_s(out, cap, _TRUNCATE, "%s", tr(Tx::PAGE_COLLAR)); return;
        case SavePlace::Gear:    _snprintf_s(out, cap, _TRUNCATE, "%s", tr(Tx::PAGE_GEAR)); return;
        case SavePlace::Chapter: _snprintf_s(out, cap, _TRUNCATE, "%s", tr(Tx::PLACE_CHAPTER)); return;
        case SavePlace::Map: {
            const Kind* k = lookup(p.kind);
            const char* node = k ? tr(k->label) : tr(Tx::UNKNOWN);
            _snprintf_s(out, cap, _TRUNCATE, "%s · %s%s", p.map[0] ? map_name(p.map) : "?", node, p.done ? "" : tr(Tx::PLACE_UNFINISHED));
            return;
        }
        default:
            // a node of a map whose kind has no label yet: the map is still worth saying ("下水道 · 未知"); anything else is just unknown
            if (p.map[0] && p.kind[0]) _snprintf_s(out, cap, _TRUNCATE, "%s · %s", map_name(p.map), tr(Tx::UNKNOWN));
            else _snprintf_s(out, cap, _TRUNCATE, "%s", tr(Tx::UNKNOWN));
            return;
    }
}

} // namespace mgmp
