// mgmp_areagate.cpp -- see mgmp_areagate.h.
#include "mgmp_areagate.h"

#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_room.h"
#include "mgmp_session.h"
#include "mgmp_unlocks.h"

namespace mgmp {
namespace {

// The areas a player can be led into, with the save flag (mapflag_<flag>) that unlocks each. Order = the overlay's name table (mgmp_menu).
struct Area { const char* id; const char* flag; };
const Area kAreas[] = {
    { "sewers",     "SewersUnlocked" },     { "junkyard",  "JunkyardUnlocked" },  { "caves",    "CavesUnlocked" },
    { "boneyard",   "BoneyardUnlocked" },   { "meatworld", "MeatWorldUnlocked" }, { "desert",   "DesertUnlocked" },
    { "bunker",     "BunkerUnlocked" },     { "crater",    "CraterUnlocked" },    { "core",     "CoreUnlocked" },
    { "moon",       "MoonUnlocked" },       { "dimensionx","DimensionXUnlocked" },{ "lab",      "LabUnlocked" },
    { "iceage",     "IceAgeUnlocked" },     { "future",    "FutureUnlocked" },    { "jurassic", "JurassicUnlocked" },
    { "theend",     "TheEndUnlocked" },     { "endoftime", "EndOfTimeUnlocked" },
};
constexpr int kAreaCount = (int)(sizeof(kAreas) / sizeof(kAreas[0]));

// MapNode (MapNode load from GON, 0x140226BA0: `next_map` is copied into +0x50; mgmp_follow: the type at +0x138, 3 = exit).
constexpr uintptr_t kNode_NextMap = 0x50;
constexpr uintptr_t kNode_Type    = 0x138;
constexpr uint32_t  kNodeExit     = 3;
constexpr uintptr_t kMap_NodeCount = 124, kMap_NodeData = 128;      // MapScreen's node vector (mgmp_follow)
// A WorldEvent option entry (240 bytes) keeps its own GON node at +0xE8: the option builder 0x14091A7E0 walks the entries from
// WorldEvent+0xE0 and asks [entry+0xE8] for "hint_chapter_exit".
constexpr uintptr_t kOpt_Gon = 0xE8;
// glaiel::GonObject, 0xB0 bytes: children a vector at +0x38/+0x40 (the game divides the span by 0xB0), name std::string +0x88, type +0xA8,
// a string value std::string +0x68 for the types 1, 2 and 5 (the reader at 0x14091AF15 copies +0x68 for exactly those).
constexpr uintptr_t kGon_Kids = 0x38, kGon_KidsEnd = 0x40, kGon_Str = 0x68, kGon_Name = 0x88, kGon_Type = 0xA8, kGon_Size = 0xB0;

constexpr ULONGLONG kReportEveryMs = 3000;

struct State {
    // HOST: what each client reported (by peer id)
    uint32_t flags[kMaxPeers] = {};
    bool     have[kMaxPeers]  = {};
    bool     dont_ask = false;          // "do not ask again in this game": this process's lifetime
    // the question
    bool     open = false, via_event = false;
    int      area = -1;
    char     target[48] = {};
    void*    map = nullptr; void* node = nullptr; uint32_t node_index = 0;
    void*    event = nullptr; uint32_t option = 0;
    uint8_t  missing[kMaxPeers] = {}; uint8_t n_missing = 0;
    // the answer, taken by the next update of the screen it belongs to
    bool     go_node = false, go_event = false;
    void*    pass_node = nullptr;
    void*    pass_event = nullptr; uint32_t pass_option = 0; bool pass_event_on = false;
    // CLIENT: the report
    bool     sent = false; uint32_t last_sent = 0; ULONGLONG last_check = 0;
} g;

bool host_room() { return net_active() && net_role() == NetRole::Host && net_peer_count() >= 2; }

// "sewers.gon", "data/maps/sewers.gon", "sewers" -> the area's index
int area_of(const char* text) {
    if (!text || !text[0]) return -1;
    const char* base = text;
    for (const char* p = text; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    char id[48] = {}; size_t n = 0;
    for (const char* p = base; *p && *p != '.' && n + 1 < sizeof(id); ++p) id[n++] = (char)tolower((unsigned char)*p);
    for (int i = 0; i < kAreaCount; ++i) if (strcmp(kAreas[i].id, id) == 0) return i;
    return -1;
}

bool gon_kids(const uint8_t* gon, const uint8_t*& begin, uint32_t& n) {
    begin = nullptr; n = 0;
    const uint8_t* end = nullptr;
    if (!gon || !mem_read(gon + kGon_Kids, &begin, sizeof(begin)) || !mem_read(gon + kGon_KidsEnd, &end, sizeof(end)) || !begin || end < begin) return false;
    const uintptr_t bytes = (uintptr_t)(end - begin);
    if (bytes % kGon_Size) return false;
    n = (uint32_t)(bytes / kGon_Size);
    return n <= 1024;
}
bool gon_string(const uint8_t* node, char* out, size_t cap) {
    int32_t type = 0;
    if (!mem_read(node + kGon_Type, &type, sizeof(type)) || !(type == 1 || type == 2 || type == 5)) return false;
    return mem_read_std_string(node + kGon_Str, out, cap) && out[0];
}
// The first `key` child (depth first, `depth` levels down) that holds a string.
bool gon_find(const uint8_t* gon, const char* key, char* out, size_t cap, int depth) {
    const uint8_t* kids = nullptr; uint32_t n = 0;
    if (!gon_kids(gon, kids, n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        char name[48] = {};
        const uint8_t* kid = kids + (uintptr_t)i * kGon_Size;
        if (mem_read_std_string(kid + kGon_Name, name, sizeof(name)) && strcmp(name, key) == 0 && gon_string(kid, out, cap)) return true;
    }
    if (depth <= 0) return false;
    for (uint32_t i = 0; i < n; ++i)
        if (gon_find(kids + (uintptr_t)i * kGon_Size, key, out, cap, depth - 1)) return true;
    return false;
}

uint32_t index_of(void* map, void* node) {
    uint32_t count = 0; void** data = nullptr;
    if (!mem_read((const uint8_t*)map + kMap_NodeCount, &count, sizeof(count)) || !mem_read((const uint8_t*)map + kMap_NodeData, &data, sizeof(data)) || !data || count > 4096) return ~0u;
    for (uint32_t i = 0; i < count; ++i) { void* p = nullptr; if (mem_read(data + i, &p, sizeof(p)) && p == node) return i; }
    return ~0u;
}
void* node_at(void* map, uint32_t index) {
    uint32_t count = 0; void** data = nullptr; void* p = nullptr;
    if (!mem_read((const uint8_t*)map + kMap_NodeCount, &count, sizeof(count)) || !mem_read((const uint8_t*)map + kMap_NodeData, &data, sizeof(data)) || !data || index >= count) return nullptr;
    return mem_read(data + index, &p, sizeof(p)) ? p : nullptr;
}

// Who of the other players lacks the area's flag. A player who never reported counts as lacking it: the question is the safe side.
uint8_t find_missing(int area, uint8_t* out, char* text, size_t cap) {
    uint8_t ids[kMaxPeers] = {};
    uint8_t n = 0;
    text[0] = 0; size_t w = 0;
    const int bit = unlocks_flag_bit(kAreas[area].flag);
    if (bit < 0 || !net_peer_ids(ids, kMaxPeers)) return 0;
    const uint8_t self = net_self();
    const uint8_t members = net_peer_count() < kMaxPeers ? net_peer_count() : kMaxPeers;
    for (uint8_t i = 0; i < members; ++i) {
        const uint8_t id = ids[i];
        if (id == self || id >= kMaxPeers) continue;
        const bool known = g.have[id];
        const bool has = known && (g.flags[id] >> bit & 1u);
        const int k = _snprintf_s(text + w, cap - w, _TRUNCATE, "%speer %u %s", w ? ", " : "", (unsigned)id, !known ? "never reported" : has ? "has it" : "LACKS it");
        if (k > 0) w += (size_t)k;
        if (!has) out[n++] = id;
    }
    return n;
}

bool ask(int area, bool via_event, const char* target, void* map, void* node, void* event, uint32_t option) {
    uint8_t missing[kMaxPeers] = {};
    char who[240];
    const uint8_t n = find_missing(area, missing, who, sizeof(who));
    const char* how = via_event ? "an event option" : "a map exit";
    if (!n) {
        log_line("AREAGATE", "the host enters '%s' (%s, flag mapflag_%s) -- every player has it (%s)", kAreas[area].id, how, kAreas[area].flag, who);
        return false;
    }
    if (g.dont_ask) {
        log_line_lvl(LogLevel::Warn, "AREAGATE", "the host enters '%s' (%s), which %u player(s) have not unlocked (%s) -- not asked: the host chose not to be asked again in this game",
                     kAreas[area].id, how, (unsigned)n, who);
        return false;
    }
    g.open = true; g.via_event = via_event; g.area = area;
    strncpy_s(g.target, sizeof(g.target), target ? target : "", _TRUNCATE);
    g.map = map; g.node = node; g.node_index = map && node ? index_of(map, node) : ~0u;
    g.event = event; g.option = option;
    memcpy(g.missing, missing, sizeof(missing)); g.n_missing = n;
    g.go_node = g.go_event = false;
    log_line_lvl(LogLevel::Warn, "AREAGATE", "HELD: the host is about to enter '%s' (%s '%s'%s), which %u player(s) have not unlocked (%s) -- asking the host first",
                 kAreas[area].id, how, g.target, via_event ? "" : (g.node_index != ~0u ? "" : ", node index unknown"), (unsigned)n, who);
    return true;
}

} // namespace

void areagate_tick() {
    if (!net_active()) {
        if (g.sent) g.sent = false;
        for (bool& h : g.have) h = false;
        if (g.open) { g.open = false; log_line("AREAGATE", "the room is gone -- the question is dropped"); }
        g.go_node = g.go_event = false;
        return;
    }
    if (net_role() == NetRole::Host) {
        // forget a player who left (a new player in the same slot must report its own flags)
        uint8_t ids[kMaxPeers] = {};
        bool present[kMaxPeers] = {};
        if (net_peer_ids(ids, kMaxPeers)) for (uint8_t i = 0; i < net_peer_count() && i < kMaxPeers; ++i) if (ids[i] < kMaxPeers) present[ids[i]] = true;
        for (uint8_t p = 0; p < kMaxPeers; ++p) if (g.have[p] && !present[p]) { g.have[p] = false; log_line("AREAGATE", "peer %u left -- its map flags are forgotten", (unsigned)p); }
        if (g.open && net_peer_count() < 2) { g.open = false; log_line("AREAGATE", "the other players left -- the question is dropped"); }
        return;
    }
    // CLIENT: report this save's map flags when they change (and once per connection)
    if (net_role() != NetRole::Client || !session_ready()) return;
    // NOT IN A BATTLE: the flags are read through the game's property getter, whose hook records every read made in a battle in the
    // per-action unlock-query digest (mgmp_unlocks) -- reads the host does not make would be a false UQ MISMATCH. The report waits for the map.
    if (lockstep_in_battle()) return;
    const ULONGLONG now = GetTickCount64();
    if (g.sent && now - g.last_check < kReportEveryMs) return;
    g.last_check = now;
    uint32_t flags = 0;
    if (!unlocks_mapflags_try(flags)) return;            // no save loaded yet
    if (g.sent && flags == g.last_sent) return;
    ChapterMsg m{};
    m.kind = kChapterFlags; m.generation = 1; m.act = 0; m.difficulty = 0; m.mapflags = flags;
    if (!net_send_chapter(m)) return;
    log_line("AREAGATE", "-> this save's map flags reported to the host: %08x%s", (unsigned)flags, g.sent ? " (changed)" : "");
    g.sent = true; g.last_sent = flags;
}

void areagate_on_flags(uint8_t from, uint32_t flags, const char* how) {
    if (net_role() != NetRole::Host || from >= kMaxPeers || from == net_self()) return;
    if (!g.have[from] || g.flags[from] != flags)
        log_line("AREAGATE", "<- peer %u's map flags (%s): %08x", (unsigned)from, how ? how : "?", (unsigned)flags);
    g.flags[from] = flags; g.have[from] = true;
}

bool areagate_hold_node(void* map, void* node) {
    if (!map || !node || !host_room()) return false;
    if (g.pass_node && node == g.pass_node) { g.pass_node = nullptr; return false; }   // the one the host confirmed
    uint32_t type = 0;
    if (!mem_read((const uint8_t*)node + kNode_Type, &type, sizeof(type)) || type != kNodeExit) {
        if (g.open) { log_line("AREAGATE", "a map click while the question is up -- held"); return true; }
        return false;
    }
    if (g.open) { log_line("AREAGATE", "another exit was clicked while the question is up -- held"); return true; }
    char next[96] = {};
    if (!mem_read_std_string((const uint8_t*)node + kNode_NextMap, next, sizeof(next)) || !next[0]) {
        log_line("AREAGATE", "an exit node with no readable next_map -- not asked");
        return false;
    }
    const int area = area_of(next);
    if (area < 0) { log_line("AREAGATE", "an exit to '%s', which is not an area of the table -- not asked", next); return false; }
    return ask(area, false, next, map, node, nullptr, 0);
}

void* areagate_take_confirmed_node(void* map) {
    if (!g.go_node) return nullptr;
    g.go_node = false;
    if (map != g.map) { log_line_lvl(LogLevel::Warn, "AREAGATE", "the map changed before the confirmed exit was entered -- nothing entered"); return nullptr; }
    if (g.node_index != ~0u && node_at(map, g.node_index) != g.node) {
        log_line_lvl(LogLevel::Warn, "AREAGATE", "the confirmed exit is no longer node %u of this map -- nothing entered", g.node_index);
        return nullptr;
    }
    log_line("AREAGATE", "-> entering the confirmed exit '%s' (node %u)", g.target, g.node_index);
    g.pass_node = g.node;
    return g.node;
}

bool areagate_hold_event(void* world_event, uint32_t index, const uint8_t* entry) {
    if (!world_event || !entry || !host_room()) return false;
    if (g.pass_event_on && world_event == g.pass_event && index == g.pass_option) { g.pass_event_on = false; return false; }
    if (g.open) { log_line("AREAGATE", "an event click while the question is up -- held"); return true; }
    const uint8_t* gon = nullptr;
    if (!mem_read(entry + kOpt_Gon, &gon, sizeof(gon)) || !gon) return false;
    char target[64] = {};
    // the hint the game itself shows ("hint_chapter_exit iceage"), else a begin_chapter anywhere in the option's result
    if (!gon_find(gon, "hint_chapter_exit", target, sizeof(target), 0) && !gon_find(gon, "begin_chapter", target, sizeof(target), 4)) return false;
    if (_stricmp(target, "home") == 0) return false;
    const int area = area_of(target);
    if (area < 0) { log_line("AREAGATE", "an event option leads to '%s', which is not an area of the table -- not asked", target); return false; }
    return ask(area, true, target, nullptr, nullptr, world_event, index);
}

bool areagate_take_confirmed_event(void* world_event, uint32_t& index) {
    if (!g.go_event) return false;
    if (world_event != g.event) {
        g.go_event = false;
        log_line_lvl(LogLevel::Warn, "AREAGATE", "the event screen changed before the confirmed option was committed -- nothing committed");
        return false;
    }
    g.go_event = false;
    index = g.option;
    g.pass_event = world_event; g.pass_option = index; g.pass_event_on = true;
    log_line("AREAGATE", "-> committing the confirmed event option %u ('%s')", index, g.target);
    return true;
}

bool areagate_view(AreaGateView& out) {
    out = AreaGateView{};
    if (!g.open) return false;
    out.area = g.area;
    out.via_event = g.via_event;
    strncpy_s(out.area_id, sizeof(out.area_id), g.area >= 0 ? kAreas[g.area].id : "?", _TRUNCATE);
    for (uint8_t i = 0; i < g.n_missing && i < kAreaGateMaxNames; ++i) {
        if (!room_peer_name(g.missing[i], out.names[out.n], sizeof(out.names[0])) || !out.names[out.n][0]) {
            const int row = room_row_of_peer(g.missing[i]);
            _snprintf_s(out.names[out.n], sizeof(out.names[0]), _TRUNCATE, "P%d", row >= 0 ? row + 1 : (int)g.missing[i] + 1);
        }
        ++out.n;
    }
    return true;
}

void areagate_answer(bool go, bool dont_ask_again) {
    if (!g.open) return;
    g.open = false;
    if (dont_ask_again && !g.dont_ask) { g.dont_ask = true; log_line("AREAGATE", "the host chose not to be asked again until the game is restarted"); }
    if (!go) { log_line("AREAGATE", "the host cancelled entering '%s' -- nothing happens", g.target); return; }
    log_line_lvl(LogLevel::Warn, "AREAGATE", "the host continues into '%s' although %u player(s) have not unlocked it", g.target, (unsigned)g.n_missing);
    if (g.via_event) g.go_event = true; else g.go_node = true;
}

} // namespace mgmp
