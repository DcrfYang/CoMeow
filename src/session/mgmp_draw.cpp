// mgmp_draw.cpp -- see mgmp_draw.h.
#include "mgmp_draw.h"

#include <windows.h>
#include <cmath>
#include <cstring>

#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_page.h"

namespace mgmp {
namespace {

std::vector<DrawStroke> g_strokes;
DrawTool  g_tool = DrawTool::None;
uint32_t  g_next_id = 0;
bool      g_pen = false;            // a stroke of this player's is being drawn
uint32_t  g_pen_id = 0;
size_t    g_pen_sent = 0;           // its points already sent
ULONGLONG g_pen_flush = 0;
uint64_t  g_battle_seen = 0;        // the last battle this peer was in: a new one takes the old one's lines away

constexpr float kStageW = 1920.0f, kStageH = 1080.0f;

uint16_t to_u16(float f) {
    if (!(f > 0.0f)) return 0;
    if (f >= 1.0f) return 65535;
    return (uint16_t)(f * 65535.0f + 0.5f);
}

DrawStroke* find(uint8_t owner, uint32_t id) {
    for (auto& s : g_strokes) if (s.owner == owner && s.id == id) return &s;
    return nullptr;
}

DrawStroke& add_stroke(uint8_t owner, uint32_t id, uint8_t layer, uint64_t scope) {
    if ((int)g_strokes.size() >= kDrawMaxStrokes) {
        // the oldest that is not the one being drawn here
        for (size_t i = 0; i < g_strokes.size(); ++i)
            if (!(g_pen && g_strokes[i].owner == net_self() && g_strokes[i].id == g_pen_id)) { g_strokes.erase(g_strokes.begin() + i); break; }
    }
    DrawStroke s; s.owner = owner; s.id = id; s.layer = layer; s.scope = scope;
    g_strokes.push_back(std::move(s));
    return g_strokes.back();
}

void remove_if_owner(uint8_t owner, uint8_t layer, bool any_layer) {
    size_t w = 0;
    for (size_t i = 0; i < g_strokes.size(); ++i) {
        const DrawStroke& s = g_strokes[i];
        const bool go = s.owner == owner && (any_layer || s.layer == layer);
        if (!go) { if (w != i) g_strokes[w] = std::move(g_strokes[i]); ++w; }
    }
    g_strokes.resize(w);
}

// The pen's pending points, in chunks. `force`: whatever is pending, now.
void flush(bool force) {
    if (!g_pen) return;
    DrawStroke* s = find(net_self(), g_pen_id);
    if (!s) { g_pen = false; return; }
    const ULONGLONG t = GetTickCount64();
    while (g_pen_sent < s->pts.size()) {
        const size_t pending = s->pts.size() - g_pen_sent;
        if (!force && pending < kDrawChunk && t - g_pen_flush < kDrawFlushMs) return;
        DrawMsg m; m.kind = kDrawPoints; m.layer = s->layer; m.stroke = s->id; m.scope = s->scope;
        m.count = (uint8_t)(pending < kDrawChunk ? pending : kDrawChunk);
        for (uint32_t i = 0; i < m.count; ++i) m.pts[i] = s->pts[g_pen_sent + i];
        net_send_draw(m);
        g_pen_sent += m.count;
        g_pen_flush = t;
    }
}

void add_point(DrawStroke& s, DrawPt p) { s.pts.push_back(p); s.cut.push_back(0); }

bool shows(const DrawStroke& s, int layer, uint64_t scope) {
    return (int)s.layer == layer && (layer != kDrawLayerBattle || s.scope == scope);
}

// THE ERASER'S CIRCLE: everything of `s` within `r` of (cx, cy) goes -- the points inside, and of every segment the part inside; a
// segment that crosses the circle is cut at the circle and the two outer pieces stay. Stage units, in doubles, from the stored
// integers: every peer runs this on the same integers and gets the same pieces. Returns whether anything went.
bool cut_circle(DrawStroke& s, double cx, double cy, double r) {
    const size_t n = s.pts.size();
    if (!n) return false;
    auto X = [&](size_t i) { return s.pts[i].x / 65535.0 * kStageW; };
    auto Y = [&](size_t i) { return s.pts[i].y / 65535.0 * kStageH; };
    auto q = [](double v, double full) {
        const double f = v / full;
        return (uint16_t)(f <= 0 ? 0 : f >= 1 ? 65535 : (uint32_t)(f * 65535.0 + 0.5));
    };
    std::vector<DrawPt> out; std::vector<uint8_t> oc;
    out.reserve(n + 4); oc.reserve(n + 4);
    bool changed = false;
    auto emit = [&](DrawPt p, bool joined) { out.push_back(p); oc.push_back(joined ? 0 : 1); };
    for (size_t i = 0; i < n; ++i) {
        const double bx = X(i), by = Y(i);
        if (i == 0 || (i < s.cut.size() && s.cut[i])) {        // a run starts here
            const double dx = bx - cx, dy = by - cy;
            if (dx * dx + dy * dy <= r * r) changed = true;
            else emit(s.pts[i], false);
            continue;
        }
        const double ax = X(i - 1), ay = Y(i - 1);
        const double dx = bx - ax, dy = by - ay, fx = ax - cx, fy = ay - cy;
        const double A = dx * dx + dy * dy, B = 2 * (fx * dx + fy * dy), C = fx * fx + fy * fy - r * r;
        double t1 = 2, t2 = 2;                                  // the part inside: t in [t1, t2] (none)
        if (A > 0) {
            const double D = B * B - 4 * A * C;
            if (D >= 0) { const double sq = std::sqrt(D); t1 = (-B - sq) / (2 * A); t2 = (-B + sq) / (2 * A); }
        } else if (C <= 0) { t1 = 0; t2 = 1; }
        if (t2 < 0 || t1 > 1) { emit(s.pts[i], true); continue; }    // clear of the circle: a (outside, already out) to b
        changed = true;
        if (t1 > 0) emit({ q(ax + t1 * dx, kStageW), q(ay + t1 * dy, kStageH) }, true);   // a .. the circle
        if (t2 < 1) {                                                                       // the circle .. b
            emit({ q(ax + t2 * dx, kStageW), q(ay + t2 * dy, kStageH) }, false);
            emit(s.pts[i], true);
        }
    }
    if (!changed) return false;
    s.pts.swap(out); s.cut.swap(oc);
    return true;
}

// The circle (c, r in 1/16 stage pixels) over `owner`'s strokes on (layer, scope); the stroke this player is drawing is left alone.
// Strokes left with nothing go. Returns how many strokes changed.
int erase_circle(uint8_t owner, uint8_t layer, uint64_t scope, DrawPt c, uint16_t r16) {
    const double cx = c.x / 65535.0 * kStageW, cy = c.y / 65535.0 * kStageH, r = r16 / 16.0;
    int changed = 0;
    for (size_t i = 0; i < g_strokes.size();) {
        DrawStroke& s = g_strokes[i];
        if (s.owner == owner && shows(s, layer, scope) && !(g_pen && owner == net_self() && s.id == g_pen_id) && cut_circle(s, cx, cy, r)) {
            ++changed;
            if (s.pts.empty()) { g_strokes.erase(g_strokes.begin() + i); continue; }
        }
        ++i;
    }
    return changed;
}

} // namespace

bool draw_available() { return net_active() && net_peer_count() >= 2; }

int draw_visible_layer(uint64_t* scope) {
    if (scope) *scope = 0;
    if (!draw_available()) return -1;
    if (page_button_live("Button_PauseMenu_")) return -1;
    if (lockstep_in_battle()) {
        if (scope) *scope = lockstep_battle_id();
        return kDrawLayerBattle;
    }
    if (page_self() != PageState::InGame) return -1;
    // a battle being built (its buttons are up before the lockstep calls it a battle), or a screen over the map
    static const char* const kNotTheMap[] = { "Combat_", "Event_", "Shop_", "InventoryButton_", "EquippedButton_" };
    for (const char* p : kNotTheMap) if (page_button_live(p)) return -1;
    // the map's own buttons, once the game has been seen to have them: no map button, no map
    if ((page_button_ever("Map_") || page_button_ever("MapNode_")) && !page_button_live("Map_") && !page_button_live("MapNode_")) return -1;
    return kDrawLayerMap;
}

DrawTool draw_tool() { return g_tool; }

void draw_set_tool(DrawTool t) {
    if (t == g_tool) return;
    if (g_pen) draw_pen_up();
    g_tool = t;
    log_line("DRAW", "tool: %s", t == DrawTool::Pen ? "pen" : t == DrawTool::Eraser ? "eraser" : "none");
}

void draw_pen_down(float x, float y) {
    if (g_pen) draw_pen_up();
    uint64_t scope = 0;
    const int layer = draw_visible_layer(&scope);
    if (layer < 0) return;
    if (!g_next_id) g_next_id = (uint32_t)(GetTickCount64() & 0x00FFFFFF) << 8;   // a restarted process does not reuse the ids it sent before
    g_pen_id = ++g_next_id;
    DrawStroke& s = add_stroke(net_self(), g_pen_id, (uint8_t)layer, scope);
    add_point(s, { to_u16(x), to_u16(y) });
    g_pen = true; g_pen_sent = 0; g_pen_flush = 0;
    flush(false);
}

void draw_pen_move(float x, float y) {
    if (!g_pen) return;
    DrawStroke* s = find(net_self(), g_pen_id);
    if (!s) { g_pen = false; return; }
    if (s->pts.size() >= kDrawMaxPoints) return;
    add_point(*s, { to_u16(x), to_u16(y) });
    flush(false);
}

void draw_pen_up() {
    if (!g_pen) return;
    flush(true);
    g_pen = false;
}

bool draw_pen_active() { return g_pen; }

int draw_erase_at(float x, float y, float radius) {
    uint64_t scope = 0;
    const int layer = draw_visible_layer(&scope);
    if (layer < 0 || !(radius > 0)) return 0;
    // the circle in the integers that travel, so this screen cuts exactly what the others will
    const DrawPt c{ to_u16(x), to_u16(y) };
    const uint16_t r16 = (uint16_t)(radius * 16.0f + 0.5f > 65535.0f ? 65535 : radius * 16.0f + 0.5f);
    const int changed = erase_circle(net_self(), (uint8_t)layer, layer == kDrawLayerBattle ? scope : 0, c, r16);
    if (changed) {   // nothing of ours under it: nothing to tell
        DrawMsg m; m.kind = kDrawEraseAt; m.layer = (uint8_t)layer; m.scope = layer == kDrawLayerBattle ? scope : 0;
        m.count = 1; m.pts[0] = c; m.radius = r16;
        net_send_draw(m);
    }
    return changed;
}

void draw_clear_own() {
    uint64_t scope = 0;
    const int layer = draw_visible_layer(&scope);
    if (layer < 0) return;
    if (g_pen) draw_pen_up();
    remove_if_owner(net_self(), (uint8_t)layer, false);
    DrawMsg m; m.kind = kDrawClear; m.layer = (uint8_t)layer; m.scope = scope;
    net_send_draw(m);
    log_line("DRAW", "this player's lines on the %s are cleared", layer == kDrawLayerBattle ? "battle" : "map");
}

void draw_tick() {
    // Out of the run: the map's lines (and any battle's) belong to it.
    const PageState p = page_self();
    if (p == PageState::MainMenu || p == PageState::SaveSlots || p == PageState::House || p == PageState::Chapter || !draw_available()) {
        if (!g_strokes.empty()) { g_strokes.clear(); log_line("DRAW", "out of the run: every line is let go"); }
        g_pen = false; g_battle_seen = 0;
        if (g_tool != DrawTool::None) draw_set_tool(DrawTool::None);
        return;
    }
    // A new battle: the lines of the one before it go (including any of the next one's that arrived early stay).
    if (lockstep_in_battle()) {
        const uint64_t b = lockstep_battle_id();
        if (b && b != g_battle_seen) {
            g_battle_seen = b;
            size_t w = 0;
            for (size_t i = 0; i < g_strokes.size(); ++i) {
                const bool keep = g_strokes[i].layer != kDrawLayerBattle || g_strokes[i].scope == b;
                if (keep) { if (w != i) g_strokes[w] = std::move(g_strokes[i]); ++w; }
            }
            g_strokes.resize(w);
        }
    }
    // The pen lifts by itself when what it draws on is no longer showing.
    if (g_pen) {
        uint64_t scope = 0;
        const int layer = draw_visible_layer(&scope);
        const DrawStroke* s = find(net_self(), g_pen_id);
        if (!s || layer != (int)s->layer || (layer == kDrawLayerBattle && scope != s->scope)) draw_pen_up();
        else flush(false);
    }
}

void draw_on_message(uint8_t from, const DrawMsg& m) {
    if (from >= kMaxPeers || from == net_self()) return;
    switch (m.kind) {
        case kDrawPoints: {
            DrawStroke* s = find(from, m.stroke);
            if (!s) s = &add_stroke(from, m.stroke, m.layer, m.scope);
            for (uint32_t i = 0; i < m.count && s->pts.size() < kDrawMaxPoints; ++i) add_point(*s, m.pts[i]);
            break;
        }
        case kDrawEraseAt:
            if (m.count == 1 && m.radius) erase_circle(from, m.layer, m.scope, m.pts[0], m.radius);
            break;
        case kDrawErase:
            for (size_t i = 0; i < g_strokes.size(); ++i)
                if (g_strokes[i].owner == from && g_strokes[i].id == m.stroke) { g_strokes.erase(g_strokes.begin() + i); break; }
            break;
        case kDrawClear:
            remove_if_owner(from, m.layer, false);
            log_line("DRAW", "peer %u cleared its lines on the %s", (unsigned)from, m.layer == kDrawLayerBattle ? "battle" : "map");
            break;
        default: break;
    }
}

void draw_reset() {
    g_strokes.clear();
    g_pen = false; g_battle_seen = 0;
    g_tool = DrawTool::None;
}

int draw_stroke_count() { return (int)g_strokes.size(); }
const DrawStroke& draw_stroke(int index) {
    static const DrawStroke kNone;
    return index >= 0 && index < (int)g_strokes.size() ? g_strokes[index] : kNone;
}

} // namespace mgmp
