#include "mgmp_tilesync.h"
#include "mgmp_unitprobe.h"          // unit_rtti_name
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_rng.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

constexpr uintptr_t kBoard_Cells = 0xB0;   // 16 bytes a cell: +4 count, +8 the entries (position components)
constexpr uintptr_t kBoard_W     = 0xB8;
constexpr uintptr_t kBoard_H     = 0xBC;
constexpr uintptr_t kPos_Layer   = 0x70;   // 0 = the ground (what TileTrail changes); 5 counts as every layer in the game's own lookups
constexpr uintptr_t kPos_Owner   = 0xA0;
constexpr uintptr_t kTile_Elem   = 0x130;  // +0x130 / +0x134: the elements the tile gives a unit standing on it
constexpr uint32_t  kCellScan    = 64;     // entries looked at per cell

struct Ground { const void* obj = nullptr; uint32_t count = 0; };

bool board_size(const void* board, uint32_t& w, uint32_t& h) {
    w = h = 0;
    return board && mem_read((const uint8_t*)board + kBoard_W, &w, 4) && mem_read((const uint8_t*)board + kBoard_H, &h, 4) && w && h && w <= 255 && h <= 255;
}

// The ground object of cell (x, y): the first layer-0 entry, and how many layer-0 entries the cell holds.
Ground ground_of(const void* board, uint32_t x, uint32_t y, uint32_t w) {
    Ground g;
    const uint8_t* cells = nullptr;
    if (!mem_read((const uint8_t*)board + kBoard_Cells, &cells, 8) || !cells) return g;
    const uint8_t* cell = cells + ((size_t)y * w + x) * 16;
    uint32_t n = 0; const void* const* data = nullptr;
    if (!mem_read(cell + 4, &n, 4) || !mem_read(cell + 8, &data, 8) || !data) return g;
    for (uint32_t i = 0; i < n && i < kCellScan; ++i) {
        const uint8_t* p = nullptr; uint8_t layer = 0xFF; const void* owner = nullptr;
        if (!mem_read(data + i, &p, 8) || !p || !mem_read(p + kPos_Layer, &layer, 1) || !mem_read(p + kPos_Owner, &owner, 8) || !owner || layer != 0) continue;
        if (!g.obj) g.obj = owner;
        ++g.count;
    }
    return g;
}

uint8_t name_index(TilesMsg& m, const char* name) {
    for (uint32_t i = 0; i < m.nnames; ++i) if (strcmp(m.names[i], name) == 0) return (uint8_t)i;
    if (m.nnames >= kTileNamesMax) return kTileNone;
    strncpy_s(m.names[m.nnames], name, _TRUNCATE);
    return m.nnames++;
}
const char* cell_name(const TilesMsg& m, uint32_t c) { return m.cell[c] < m.nnames ? m.names[m.cell[c]] : ""; }

// The tile of class `name` made on the cell of `tile`, the game's way. The new tile object, or null (refused / not available / faulted).
void* replace_tile(void* tile, const char* name, int& err) {
    err = 0;
    const uintptr_t fn = addr_of_call(C_TileReplace), sctor = addr_of_call(C_StrFromCStr);
    if (!fn || !sctor) { err = -2; return nullptr; }
    uint64_t* stream = rng_global_stream();
    uint64_t saved[4] = {};
    if (stream) mem_read(stream, saved, sizeof(saved));       // what the new tile draws is not the shared stream's: the host's game drew it when it changed its tile
    void* made = nullptr;
    __try {
        alignas(16) uint8_t str[32];
        memset(str, 0, sizeof(str));
        ((void* (__fastcall*)(void*, const char*))sctor)(str, name);
        made = ((void* (__fastcall*)(void*, void*, bool))fn)(tile, str, false);   // the name by value: the callee frees it
    } __except (EXCEPTION_EXECUTE_HANDLER) { err = -1; made = nullptr; }
    if (stream) mem_write(stream, saved, sizeof(saved));
    return made;
}

// CLIENT: the host's maps of the last boundaries (raw MSG_TILES payloads, filled by the receive thread through net_host_tiles)
TilesMsg g_host, g_mine;
uint8_t  g_buf[kSendTiles];
uint64_t g_logged_battle = 0;
uint32_t g_logged_lines = 0;
constexpr uint32_t kLogCap = 400;

} // namespace

bool tiles_read(const void* board, TilesMsg& m) {
    uint32_t w = 0, h = 0;
    if (!board_size(board, w, h) || (size_t)w * h > kTilesMaxCells) return false;
    const uint64_t battle = m.battle; const uint32_t turn = m.turn;
    m = TilesMsg{};
    m.battle = battle; m.turn = turn; m.w = (uint8_t)w; m.h = (uint8_t)h;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t c = y * w + x;
            const Ground g = ground_of(board, x, y, w);
            m.cell[c] = kTileNone;
            if (!g.obj) continue;
            char cls[kTileNameLen] = {};
            if (!unit_rtti_name(g.obj, cls, sizeof(cls))) strcpy_s(cls, "?");
            m.cell[c] = name_index(m, cls);
            uint32_t em[2] = {};
            mem_read((const uint8_t*)g.obj + kTile_Elem, em, 8);
            m.m0[c] = em[0]; m.m1[c] = em[1];
            m.extra[c] = (uint8_t)(g.count > 255 ? 255 : g.count);
        }
    return true;
}

void tilesync_boundary(uint64_t battle, uint32_t turn, const void* board, bool host) {
    if (!battle || !board) return;
    if (g_logged_battle != battle) { g_logged_battle = battle; g_logged_lines = 0; }
    if (host) {
        g_mine.battle = battle; g_mine.turn = turn;
        if (!tiles_read(board, g_mine)) { if (g_logged_lines++ < kLogCap) log_line_lvl(LogLevel::Warn, "TILESYNC", "turn %u: this board's ground could not be read -- not sent", turn); return; }
        net_send_tiles(g_mine);
        return;
    }
    uint32_t n = net_host_tiles(battle, turn, g_buf, sizeof(g_buf));
    // 1.5 s, not 0.3 (0.1.197; room KL8W 11-06 19:19, battle fd4241a9: "the host's ground map of this boundary is not here (not arrived)" at turn 1, the turn disagreed --
    // the map travels right behind the host's board, and on a link of a few hundred ms round trip 0.3 s after the board is not always enough)
    for (const ULONGLONG t0 = GetTickCount64(); !n && GetTickCount64() - t0 < 1500; ) { Sleep(2); n = net_host_tiles(battle, turn, g_buf, sizeof(g_buf)); }
    if (!n || !dec_tiles(g_buf, n, g_host)) {
        if (g_logged_lines++ < kLogCap) log_line_lvl(LogLevel::Warn, "TILESYNC", "turn %u: the host's ground map of this boundary is not here (%s) -- this board's tiles are left as they are", turn, n ? "undecodable" : "not arrived");
        return;
    }
    g_mine.battle = battle; g_mine.turn = turn;
    if (!tiles_read(board, g_mine)) { if (g_logged_lines++ < kLogCap) log_line_lvl(LogLevel::Warn, "TILESYNC", "turn %u: this board's ground could not be read", turn); return; }
    if (g_mine.w != g_host.w || g_mine.h != g_host.h) {
        if (g_logged_lines++ < kLogCap) log_line_lvl(LogLevel::Error, "TILESYNC", "!! turn %u: the board is %ux%u here and %ux%u on the host -- not compared", turn, g_mine.w, g_mine.h, g_host.w, g_host.h);
        return;
    }
    uint32_t w = 0, h = 0;
    board_size(board, w, h);
    uint32_t changed = 0, failed = 0, elem_only = 0;
    char line[900]; int lw = 0; line[0] = 0;
    auto put = [&](const char* fmt, auto... a) { if (lw >= 0 && lw < (int)sizeof(line) - 100) lw += _snprintf_s(line + lw, sizeof(line) - lw, _TRUNCATE, fmt, a...); };
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t c = y * w + x;
            const char* hn = cell_name(g_host, c); const char* mn = cell_name(g_mine, c);
            if (strcmp(hn, mn) == 0) {
                if (g_host.m0[c] != g_mine.m0[c] || g_host.m1[c] != g_mine.m1[c]) {
                    ++elem_only;
                    put(" (%u,%u) %s elements %08X:%08X|%08X:%08X", x, y, mn, g_host.m0[c], g_host.m1[c], g_mine.m0[c], g_mine.m1[c]);
                }
                continue;
            }
            const Ground gnd = ground_of(board, x, y, w);
            if (!hn[0] || !gnd.obj || strcmp(hn, "?") == 0) { ++failed; put(" !(%u,%u) '%s'|'%s' (%s)", x, y, hn, mn, !hn[0] ? "no ground object on the host" : !gnd.obj ? "no ground object here" : "the host's class unknown"); continue; }
            int err = 0;
            void* made = replace_tile(const_cast<void*>(gnd.obj), hn, err);
            const Ground now = ground_of(board, x, y, w);
            char cls[kTileNameLen] = {};
            if (now.obj) unit_rtti_name(now.obj, cls, sizeof(cls));
            if (strcmp(cls, hn) == 0) { ++changed; put(" (%u,%u) '%s'->'%s'", x, y, mn, hn); }
            else { ++failed; put(" !(%u,%u) '%s' not made over '%s' (%s, the cell holds '%s')", x, y, hn, mn, err ? "faulted" : made ? "the game made another" : "the game refused", cls); }
        }
    if (!changed && !failed && !elem_only) return;
    if (g_logged_lines++ < kLogCap)
        log_line_lvl(failed ? LogLevel::Error : LogLevel::Warn, "TILESYNC", "%s turn %u: %u tile(s) made the host's, %u not, %u with the same tile and other elements (host|here) --%s",
                     failed ? "!!" : "", turn, changed, failed, elem_only, line);
}

bool tilesync_dev_set(const void* board, uint32_t x, uint32_t y, const char* name) {
    uint32_t w = 0, h = 0;
    if (!board_size(board, w, h) || x >= w || y >= h || !name || !name[0]) return false;
    const Ground g = ground_of(board, x, y, w);
    if (!g.obj) return false;
    int err = 0;
    replace_tile(const_cast<void*>(g.obj), name, err);
    const Ground now = ground_of(board, x, y, w);
    char cls[kTileNameLen] = {};
    if (now.obj) unit_rtti_name(now.obj, cls, sizeof(cls));
    log_line_lvl(LogLevel::Warn, "TILESYNC", "dev on THIS game only: cell (%u,%u) '%s' asked, the cell holds '%s' (err %d)", x, y, name, cls, err);
    return strcmp(cls, name) == 0;
}

void tilesync_dev_dump(const void* board) {
    static TilesMsg m;
    m.battle = 1; m.turn = 0;
    if (!tiles_read(board, m)) { log_line("TILESYNC", "dump: the board could not be read (%p)", board); return; }
    log_line("TILESYNC", "dump: board %p, %ux%u, %u class(es)", board, m.w, m.h, m.nnames);
    for (uint32_t y = 0; y < m.h; ++y) {
        char row[1600]; int w = 0; row[0] = 0;
        for (uint32_t x = 0; x < m.w && w >= 0 && w < (int)sizeof(row) - 60; ++x) {
            const uint32_t c = y * m.w + x;
            w += _snprintf_s(row + w, sizeof(row) - w, _TRUNCATE, " %s", cell_name(m, c)[0] ? cell_name(m, c) : "-");
            if (m.m0[c] || m.m1[c]) w += _snprintf_s(row + w, sizeof(row) - w, _TRUNCATE, "[%X:%X]", m.m0[c], m.m1[c]);
            if (m.extra[c] > 1) w += _snprintf_s(row + w, sizeof(row) - w, _TRUNCATE, "x%u", (unsigned)m.extra[c]);
        }
        log_line("TILESYNC", "  y=%2u:%s", y, row);
    }
}

} // namespace mgmp
