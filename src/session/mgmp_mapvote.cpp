// mgmp_mapvote.cpp -- see mgmp_mapvote.h.
#include "mgmp_mapvote.h"

#include <windows.h>
#include <cmath>
#include <cstring>

#include "mgmp_addresses.h"
#include "mgmp_follow.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_resolve.h"

namespace mgmp {
namespace {

constexpr uint32_t kType_Home = 4;                       // MapNode type "home": its click first asks "return home early?"
constexpr ULONGLONG kResendMs = 1500;                    // a player's own pick is said again at this rate (a lost frame, a newcomer)
constexpr ULONGLONG kClickTakeMs = 6000;                 // the host's click ran and nothing moved: the pick is dropped after this
constexpr ULONGLONG kClickTakeHomeMs = 90000;            // ... a home node waits on a confirmation popup
constexpr ULONGLONG kDebounceMs = 250;                   // one physical click can reach the callback more than once: a second call this soon is the same click
// MapNode+0x160 -> the node's swf display object: x / y at +0x70 / +0x74, scale at +0x60 / +0x64, its parent at +0x38 (the layout the treasure room's clips have too).
constexpr uintptr_t kNode_Clip = 0x160;
constexpr uintptr_t kDO_Parent = 0x38, kDO_Sx = 0x60, kDO_Sy = 0x64, kDO_X = 0x70, kDO_Y = 0x74;
// 0.1.100, from the 0.1.99 click log (17 clicks on 9 nodes, two screens, the camera all over the map): a node's x / y are map.swf units (1280x720) and the camera
// (MapScreen+0x180 -> +0x38: x / y as doubles at +0x80 / +0x88, a view size factor at +0x98 / +0xA0, 1.0 at rest) is the view's CENTRE in the same units:
//     stage = (960, 540) + 1.5 * (layer - camera)
// (0.1.99 had 1.7778 * (..) + (982, 504), fitted to four clicks that were all within 80 units of each other: right near the middle, wrong towards the edges.)
constexpr float     kStageScale = 1920.0f / 1280.0f, kStageCx = 960.0f, kStageCy = 540.0f;
constexpr float     kNodeHalfStage = 52.0f;              // a node's tile, half its size on the stage at the rest zoom
// 0.1.97 (the paw stayed where it was when the camera moved): MapScreen::update (0x38F3A0) keeps a camera at MapScreen+0x180 -> object, whose +0x38 data holds the
// position as three doubles at +0x80 / +0x88 / +0x90 and the view size at +0x98 / +0xA0, the map's bounds at MapScreen+0x188 .. 0x1A7. The nodes' x / y never change, the
// camera does, so the screen position is  world - (camera - the camera when the map was first seen)  + the calibrated offset.
constexpr uintptr_t kMap_Camera = 0x180, kCam_Data = 0x38, kCamD_X = 0x80, kCamD_Y = 0x88, kCamD_Z = 0x90, kCamD_W = 0x98, kCamD_H = 0xA0;
constexpr uintptr_t kMap_Bounds = 0x188;

SRWLOCK g_lock = SRWLOCK_INIT;
struct Lock { Lock() { AcquireSRWLockExclusive(&g_lock); } ~Lock() { ReleaseSRWLockExclusive(&g_lock); } };

struct State {
    MapVoteBook book;
    void*     map = nullptr;
    uint64_t  here_seed = 0;          // follow_here_seed() the last tick: a change is a node entered
    ULONGLONG resend_at = 0;
    uint8_t   last_n = 0;
    bool      exec = false;           // the host's click ran, the node is not entered yet
    ULONGLONG exec_at = 0;
    bool      force = false;          // the host asked to go on without the others (Ctrl)
    uint64_t  exec_seed = 0;          // the node the host's click was run for (its seed word): the party "arrives" when the marker stands on it
    bool      exec_home = false;
    ULONGLONG click_at = 0;           // this player's last accepted click (debounce)
    uint32_t  said_blocked = ~0u;
    uint32_t  geo_logged[kMaxPeers] = {};   // index + 1 of the pick whose geometry was last written to the log, per player
    ULONGLONG tick_at = 0;        // the last MapScreen::update seen by mapvote_tick (the paws show only while the map itself ticks)
    ULONGLONG over_at = 0;        // the last tick of LevelUpScreen::update (the log only: it never stops once the screen has existed)
    float     mouse_x = 0, mouse_y = 0;   // the mouse on the stage, as the overlay last saw it
    ULONGLONG mouse_at = 0;
    void*     dumped_map = nullptr;       // the map whose nodes were written to the log
    float     cam_logged_x = 1e9f, cam_logged_y = 1e9f;
    ULONGLONG cam_logged_at = 0;
    int       cam_lines = 0, cal_lines = 0;
} g;

bool engaged() { return follow_active() && net_peer_count() >= 2 && net_self() != kNoPeer; }

typedef bool (__fastcall* fn_blocked)(void* marker, void* node);
bool marker_blocks(void* marker, void* node) {
    const uintptr_t fn = addr_of_call(C_MapNodeBlocked);
    if (!fn) return false;                                // not resolved: the game's own check still drops a click it does not accept
    __try { return ((fn_blocked)fn)(marker, node); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void send_vote(uint64_t seed0, uint32_t index, bool on) {
    NodeVoteMsg m;
    m.seed0 = seed0; m.index = index; m.on = on ? 1 : 0;
    if (!net_send_nodevote(m)) log_line_lvl(LogLevel::Warn, "MAPVOTE", "!! the pick could not be sent");
}

void members(uint8_t* ids, uint8_t& n) {
    n = 0;
    uint8_t all[kMaxPeers] = {};
    if (!net_peer_ids(all, kMaxPeers)) return;
    const uint8_t count = net_peer_count() > kMaxPeers ? kMaxPeers : net_peer_count();
    for (uint8_t i = 0; i < count; ++i) if (all[i] < kMaxPeers) ids[n++] = all[i];
}

void reset_locked(const char* why) {
    if (g.book.any() || g.exec) log_line("MAPVOTE", "the picks are forgotten (%s)", why);
    g.book.clear();
    g.exec = false; g.force = false; g.said_blocked = ~0u;
    for (uint32_t& x : g.geo_logged) x = 0;
}

bool read_f(const void* o, uintptr_t off, float& v) { return mem_read((const uint8_t*)o + off, &v, sizeof(v)) && std::isfinite(v) && std::fabs(v) < 1e5f; }

// A node's position in the map layer's own units: the clip's x / y added up the parent chain (translation and scale of every level). `verbose` writes every
// step to the log.
bool node_world(void* node, float& wx, float& wy, bool verbose, uint32_t index) {
    void* clip = nullptr;
    if (!node || !mem_read((const uint8_t*)node + kNode_Clip, &clip, sizeof(clip)) || !clip) {
        if (verbose) log_line("MAPVOTE", "geometry of node %u: MapNode+0x%X holds no clip", index, (unsigned)kNode_Clip);
        return false;
    }
    void* obj = clip;
    float x = 0, y = 0;
    int depth = 0;
    for (; obj && depth < 24; ++depth) {
        float px = 0, py = 0, sx = 1, sy = 1;
        if (!read_f(obj, kDO_X, px) || !read_f(obj, kDO_Y, py) || !read_f(obj, kDO_Sx, sx) || !read_f(obj, kDO_Sy, sy)) { if (verbose) log_line("MAPVOTE", "geometry of node %u: level %d (%p) does not read", index, depth, obj); return false; }
        x = px + x * sx; y = py + y * sy;
        void* up = nullptr;
        if (!mem_read((const uint8_t*)obj + kDO_Parent, &up, sizeof(up))) { if (verbose) log_line("MAPVOTE", "geometry of node %u: the parent of level %d does not read", index, depth); return false; }
        obj = up;
    }
    if (obj) { if (verbose) log_line("MAPVOTE", "geometry of node %u: the chain is longer than 24 levels", index); return false; }
    wx = x; wy = y;
    return true;
}

bool read_d(const void* o, uintptr_t off, double& v) { return mem_read((const uint8_t*)o + off, &v, sizeof(v)) && std::isfinite(v) && std::fabs(v) < 1e7; }

// The camera's position (and view size) now; false when it does not read.
bool camera_now(void* map, double& x, double& y, double* w = nullptr, double* h = nullptr, double* z = nullptr) {
    void* cam = nullptr; void* data = nullptr;
    if (!map || !mem_read((const uint8_t*)map + kMap_Camera, &cam, sizeof(cam)) || !cam) return false;
    if (!mem_read((const uint8_t*)cam + kCam_Data, &data, sizeof(data)) || !data) return false;
    double dw = 0, dh = 0, dz = 0;
    if (!read_d(data, kCamD_X, x) || !read_d(data, kCamD_Y, y)) return false;
    read_d(data, kCamD_W, dw); read_d(data, kCamD_H, dh); read_d(data, kCamD_Z, dz);
    if (w) *w = dw;
    if (h) *h = dh;
    if (z) *z = dz;
    return true;
}

// A node's centre on the 1920x1080 stage right now, and half its size there. False when the node or the camera does not read.
bool node_stage(void* node, float& cx, float& cy, float& half, bool verbose, uint32_t index) {
    float wx = 0, wy = 0;
    if (!node_world(node, wx, wy, verbose, index)) return false;
    double camx = 0, camy = 0, vw = 1, vh = 1;
    if (!camera_now(g.map, camx, camy, &vw, &vh)) { if (verbose) log_line("MAPVOTE", "geometry of node %u: the camera does not read", index); return false; }
    // the view's size factor is 1.0 at rest; while the map zooms in (0.2 for the first frames) the picture is bigger by its inverse
    const float zoom = (vw > 0.3 && vw < 3.0) ? (float)(1.0 / vw) : 1.0f;
    const float s = kStageScale * zoom;
    cx = kStageCx + s * (wx - (float)camx);
    cy = kStageCy + s * (wy - (float)camy);
    half = kNodeHalfStage * zoom;
    // the camera's moves, to the log (rate-limited)
    const ULONGLONG now = GetTickCount64();
    if (g.cam_lines < 40 && now - g.cam_logged_at > 1000 && (std::fabs((float)camx - g.cam_logged_x) > 15 || std::fabs((float)camy - g.cam_logged_y) > 15)) {
        log_line("MAPVOTE", "camera at (%.1f, %.1f), view %.2f x %.2f", camx, camy, vw, vh);
        g.cam_logged_x = (float)camx; g.cam_logged_y = (float)camy; g.cam_logged_at = now; ++g.cam_lines;
    }
    return std::isfinite(cx) && std::isfinite(cy);
}

// This player's click on a node: the mouse is on it. Only compared with the model (the log); how far off the model is, is what a later fix needs.
void note_click(void* node, uint32_t idx) {
    if (GetTickCount64() - g.mouse_at > 500) return;
    float cx, cy, half;
    if (!node_stage(node, cx, cy, half, false, idx)) return;
    if (g.cal_lines < 40) { ++g.cal_lines; log_line("MAPVOTE", "click on node %u: mouse at stage (%.0f, %.0f), the model puts the node at (%.0f, %.0f): %+.0f / %+.0f", idx, g.mouse_x, g.mouse_y, cx, cy, g.mouse_x - cx, g.mouse_y - cy); }
}

} // namespace

bool mapvote_on_click(void* node) {
    Lock lock;
    if (!node || !engaged()) return false;
    void* map = g.map ? g.map : follow_map_screen();
    uint32_t idx = 0;
    if (!map || !follow_map_ready(map) || !follow_index_of(map, node, idx)) return false;      // not a node of this map: the game's own click
    void *marker = nullptr, *at = nullptr, *moving = nullptr, *sel = nullptr;
    if (!follow_marker(map, marker, at, moving, sel)) return false;
    if (node == at) return false;                                                               // the node the party stands on: the game clears its selection
    if (moving) return true;                                                                    // the party is walking: the game ignores a click now as well
    if (follow_node_consumed(node) || marker_blocks(marker, node)) {
        if (g.said_blocked != idx) { g.said_blocked = idx; log_line("MAPVOTE", "a click on node %u (%s): the party cannot walk there -- not a pick", idx, "blocked or behind"); }
        return true;
    }
    const ULONGLONG t_click = GetTickCount64();
    if (g.click_at && t_click - g.click_at < kDebounceMs) return true;                         // the same click again
    note_click(node, idx);
    g.click_at = t_click;
    const uint8_t self = net_self();
    const uint64_t seed = follow_node_seed(node);
    const bool now_mine = g.book.toggle(self, seed, idx);
    send_vote(seed, idx, now_mine);
    g.resend_at = GetTickCount64() + kResendMs;
    log_line("MAPVOTE", "this player %s node %u (type %u)", now_mine ? "picks" : "takes back its pick of", idx, (unsigned)follow_node_type(node));
    if (now_mine && net_role() == NetRole::Host && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
        g.force = true;
        log_line("MAPVOTE", "the host holds Ctrl: its pick is entered without waiting for the others");
    }
    return true;
}

void mapvote_on_message(uint8_t from, const NodeVoteMsg& m) {
    Lock lock;
    if (from >= kMaxPeers || from == net_self() || !engaged()) return;
    if (m.on) {
        if (g.book.put(from, m.seed0, m.index)) log_line("MAPVOTE", "peer %u picks node %u", (unsigned)from, m.index);
    } else if (g.book.is(from, m.seed0, m.index)) {
        g.book.drop(from);
        log_line("MAPVOTE", "peer %u takes back its pick of node %u", (unsigned)from, m.index);
    }
}

void mapvote_tick(void* map, MapVoteClickFn click) {
    Lock lock;
    g.map = map;
    g.tick_at = GetTickCount64();
    const uint64_t here = follow_here_seed();
    if (!engaged()) {
        if (g.book.any() || g.exec) reset_locked("the room or the follow is gone");
        g.here_seed = here;
        return;
    }
    if (here != g.here_seed) { g.here_seed = here; reset_locked("a node was entered"); }

    uint8_t ids[kMaxPeers] = {}, n = 0;
    members(ids, n);
    bool present[kMaxPeers] = {};
    for (uint8_t i = 0; i < n; ++i) present[ids[i]] = true;
    for (uint8_t p = 0; p < kMaxPeers; ++p)
        if (g.book.has(p) && !present[p]) { g.book.drop(p); log_line("MAPVOTE", "peer %u left: its pick is forgotten", (unsigned)p); }
    const ULONGLONG now = GetTickCount64();
    if (n != g.last_n) { g.last_n = n; g.resend_at = 0; }

    if (!follow_map_ready(map)) return;
    void *marker = nullptr, *at = nullptr, *moving = nullptr, *sel = nullptr;
    follow_marker(map, marker, at, moving, sel);
    if (g.dumped_map != map) {                                                                  // every node's place in the layer, for the calibration
        g.dumped_map = map;
        g.cam_lines = 0; g.cal_lines = 0; g.cam_logged_x = g.cam_logged_y = 1e9f;
        double cx = 0, cy = 0, cw = 0, ch = 0, cz = 0;
        const bool cam_ok = camera_now(map, cx, cy, &cw, &ch, &cz);
        double b[4] = {};
        for (int i = 0; i < 4; ++i) read_d(map, kMap_Bounds + 8 * i, b[i]);
        log_line("MAPVOTE", "camera: %s, at (%.1f, %.1f, %.1f), view %.1f x %.1f; bounds %.1f %.1f %.1f %.1f", cam_ok ? "read" : "NOT readable", cx, cy, cz, cw, ch, b[0], b[1], b[2], b[3]);
        const uint32_t cnt = follow_node_count();
        for (uint32_t i = 0; i < cnt && i < 64; ++i) {
            void* nd = follow_node_by_index(map, i);
            float wx = 0, wy = 0;
            const bool ok = nd && node_world(nd, wx, wy, false, i);
            log_line("MAPVOTE", "map node %u: type %u, layer (%.1f, %.1f)%s%s", i, nd ? (unsigned)follow_node_type(nd) : 0u, wx, wy, ok ? "" : " (unreadable)", nd == at ? "  <- the party stands here" : "");
        }
    }
    // a pick that names no node of this map (another map, an old one) or a node already behind the party is gone
    for (uint8_t p = 0; p < kMaxPeers; ++p) {
        if (!g.book.has(p)) continue;
        void* node = follow_node_by_index(map, g.book.v[p].index);
        if (!node || follow_node_seed(node) != g.book.v[p].seed0 || follow_node_consumed(node) || node == at) {
            log_line("MAPVOTE", "peer %u's pick of node %u no longer names a node ahead -- forgotten", (unsigned)p, g.book.v[p].index);
            g.book.drop(p);
        }
    }
    const uint8_t self = net_self();
    if (g.book.has(self) && now >= g.resend_at) {
        send_vote(g.book.v[self].seed0, g.book.v[self].index, true);
        g.resend_at = now + kResendMs;
    }
    if (net_role() != NetRole::Host || !click) return;

    // THE HOST: every player has picked (or it holds Ctrl) -- its own pick is entered
    if (g.exec) {
        if (moving) { g.exec_at = now; return; }                                                // walking: the node is entered on arrival
        if (at && follow_node_seed(at) == g.exec_seed) { g.exec_at = now; return; }             // arrived: EnterNode (or its barrier's retry) takes it from here
        const ULONGLONG limit = g.exec_home ? kClickTakeHomeMs : kClickTakeMs;
        if (now - g.exec_at > limit) {
            log_line("MAPVOTE", "the host's click did not take (the party did not move) -- its pick is taken back; it clicks again");
            if (g.book.has(self)) send_vote(g.book.v[self].seed0, g.book.v[self].index, false);
            g.book.drop(self);
            g.exec = false; g.force = false;
        }
        return;
    }
    if (!g.book.has(self) || moving) return;
    if (!g.book.all(ids, n) && !g.force) return;
    void* node = follow_node_by_index(map, g.book.v[self].index);
    if (!node || follow_node_seed(node) != g.book.v[self].seed0) return;
    g.exec = true; g.exec_at = now; g.exec_seed = g.book.v[self].seed0; g.exec_home = follow_node_type(node) == kType_Home;
    log_line("MAPVOTE", "%s: the host's pick, node %u (type %u), is entered now (%u of %u player(s) had picked)", g.force ? "forced" : "every player has picked",
             g.book.v[self].index, (unsigned)follow_node_type(node), g.book.count(ids, n), (unsigned)n);
    click(node);
}

void mapvote_note_screen_over() {
    Lock lock;
    g.over_at = GetTickCount64();
}

bool mapvote_map_ticking() {
    Lock lock;
    return GetTickCount64() - g.tick_at < 300;
}

bool mapvote_level_ticking() {
    Lock lock;
    return GetTickCount64() - g.over_at < 500;
}

void mapvote_note_mouse(float stage_x, float stage_y) {
    Lock lock;
    g.mouse_x = stage_x; g.mouse_y = stage_y; g.mouse_at = GetTickCount64();
}

void mapvote_reset(const char* why) {
    Lock lock;
    reset_locked(why ? why : "?");
}

int mapvote_marks(MapVoteMark* out, int cap) {
    Lock lock;
    if (!out || cap <= 0 || !engaged() || !g.map || !g.book.any()) return 0;
    int m = 0;
    for (uint8_t p = 0; p < kMaxPeers && m < cap; ++p) {
        if (!g.book.has(p)) continue;
        void* node = follow_node_by_index(g.map, g.book.v[p].index);
        if (!node || follow_node_seed(node) != g.book.v[p].seed0) continue;
        MapVoteMark& k = out[m];
        k.peer = p;
        const bool first = g.geo_logged[p] != g.book.v[p].index + 1;                  // a pick's place is written once, when it first needs drawing
        if (!node_stage(node, k.cx, k.cy, k.half, first, g.book.v[p].index)) continue;
        {   // a fixed pseudo-random tilt of -45 .. +45 degrees per player and pick: the same on every screen
            uint64_t h = g.book.v[p].seed0 * 0x9E3779B97F4A7C15ull + (uint64_t)(p + 1) * 0xBF58476D1CE4E5B9ull + g.book.v[p].index;
            h ^= h >> 29; h *= 0x94D049BB133111EBull; h ^= h >> 32;
            k.angle = ((int)(h % 91) - 45) * 3.14159265f / 180.0f;
        }
        if (first) log_line("MAPVOTE", "peer %u's pick of node %u: stage (%.0f, %.0f)", (unsigned)p, g.book.v[p].index, k.cx, k.cy);
        g.geo_logged[p] = g.book.v[p].index + 1;
        if (!(k.cx > -200 && k.cx < 2120 && k.cy > -200 && k.cy < 1280)) continue;
        ++m;
    }
    return m;
}

bool mapvote_counts(unsigned& have, unsigned& total, bool& i_picked) {
    Lock lock;
    have = total = 0; i_picked = false;
    if (!engaged() || !g.book.any()) return false;
    uint8_t ids[kMaxPeers] = {}, n = 0;
    members(ids, n);
    total = n; have = g.book.count(ids, n);
    i_picked = g.book.has(net_self());
    return true;
}

} // namespace mgmp
