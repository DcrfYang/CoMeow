// The drawing board's data and wire (mgmp_draw): which layer shows, the pen's chunks, the eraser, the bin, other players' lines, and
// what a new battle or the end of a run lets go of. The game and the network are stubs.
#include "../src/session/mgmp_draw.cpp"
#include <cstdio>
#include <cstdlib>
#include <algorithm>

using namespace mgmp;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

namespace {
bool     t_active = true;
uint8_t  t_peers = 2, t_self = 1;
bool     t_battle = false;
uint64_t t_battle_id = 0;
PageState t_page = PageState::InGame;
const char* t_live[8] = {};
const char* t_ever[8] = {};
std::vector<DrawMsg> sent;

bool starts(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }
void live(std::initializer_list<const char*> l) { int i = 0; for (auto* s : t_live) (void)s; for (auto& s : t_live) s = nullptr; for (auto* s : l) t_live[i++] = s; }
void ever(std::initializer_list<const char*> l) { int i = 0; for (auto& s : t_ever) s = nullptr; for (auto* s : l) t_ever[i++] = s; }
// all points of one of this player's strokes, as the others received them
std::vector<DrawPt> wire_points(uint32_t stroke) {
    std::vector<DrawPt> out;
    for (const auto& m : sent) if (m.kind == kDrawPoints && m.stroke == stroke) for (uint32_t i = 0; i < m.count; ++i) out.push_back(m.pts[i]);
    return out;
}
const DrawStroke* stroke_of(uint8_t owner, uint32_t id) {
    for (int i = 0; i < draw_stroke_count(); ++i) if (draw_stroke(i).owner == owner && draw_stroke(i).id == id) return &draw_stroke(i);
    return nullptr;
}
int runs_of(const DrawStroke& s) { int n = 0; for (size_t i = 0; i < s.pts.size(); ++i) n += i == 0 || s.cut[i]; return n; }
// the nearest any remaining LINE of `s` comes to (cx, cy), in stage pixels
double nearest(const DrawStroke& s, double cx, double cy) {
    double best = 1e9;
    for (size_t i = 0; i < s.pts.size(); ++i) {
        const double bx = s.pts[i].x / 65535.0 * 1920, by = s.pts[i].y / 65535.0 * 1080;
        if (i == 0 || s.cut[i]) { best = std::min(best, std::hypot(bx - cx, by - cy)); continue; }
        const double ax = s.pts[i - 1].x / 65535.0 * 1920, ay = s.pts[i - 1].y / 65535.0 * 1080;
        const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
        double u = l2 > 0 ? ((cx - ax) * dx + (cy - ay) * dy) / l2 : 0; u = u < 0 ? 0 : u > 1 ? 1 : u;
        best = std::min(best, std::hypot(ax + u * dx - cx, ay + u * dy - cy));
    }
    return best;
}
int count_of(uint8_t owner) { int n = 0; for (int i = 0; i < draw_stroke_count(); ++i) n += draw_stroke(i).owner == owner; return n; }
DrawMsg points(uint32_t stroke, uint8_t layer, uint64_t scope, std::initializer_list<DrawPt> p) {
    DrawMsg m; m.kind = kDrawPoints; m.layer = layer; m.stroke = stroke; m.scope = scope; m.count = (uint8_t)p.size();
    int i = 0; for (const auto& q : p) m.pts[i++] = q; return m;
}
} // namespace

namespace mgmp {
bool net_active() { return t_active; }
uint8_t net_peer_count() { return t_peers; }
uint8_t net_self() { return t_self; }
bool net_send_draw(const DrawMsg& m) { sent.push_back(m); return true; }
bool lockstep_in_battle() { return t_battle; }
uint64_t lockstep_battle_id() { return t_battle_id; }
PageState page_self() { return t_page; }
bool page_button_live(const char* p) { for (auto* s : t_live) if (s && starts(s, p)) return true; return false; }
bool page_button_ever(const char* p) { for (auto* s : t_ever) if (s && starts(s, p)) return true; return false; }
void log_line(const char*, const char*, ...) {}
}

int main() {
    // --- which layer shows ---
    CHECK(draw_visible_layer() == kDrawLayerMap);                                  // in a run, no map button ever seen: the map
    live({ "Event_ChoiceButton" }); CHECK(draw_visible_layer() == -1);             // an event over the map
    live({ "Shop_BuyButton" }); CHECK(draw_visible_layer() == -1);                 // a shop
    live({ "InventoryButton_trinket" }); CHECK(draw_visible_layer() == -1);        // the backpack
    live({ "Button_PauseMenu_Resume" }); CHECK(draw_visible_layer() == -1);        // the pause menu
    live({}); ever({ "MapNode_Battle" }); CHECK(draw_visible_layer() == -1);       // the map's buttons are known and none is up
    live({ "MapNode_Battle" }); CHECK(draw_visible_layer() == kDrawLayerMap);
    live({ "MapNode_Battle", "Event_ExitButton" }); CHECK(draw_visible_layer() == -1);
    live({ "Map_Backpack" }); CHECK(draw_visible_layer() == kDrawLayerMap);
    t_page = PageState::House; CHECK(draw_visible_layer() == -1); t_page = PageState::InGame;
    t_peers = 1; CHECK(draw_visible_layer() == -1); t_peers = 2;
    t_battle = true; t_battle_id = 0xB1;
    { uint64_t sc = 0; CHECK(draw_visible_layer(&sc) == kDrawLayerBattle && sc == 0xB1); }
    live({ "Button_PauseMenu_Resume" }); CHECK(draw_visible_layer() == -1); live({ "Map_Backpack" });
    t_battle = false;

    // --- the pen: every point reaches the wire, in chunks, the first at once ---
    draw_set_tool(DrawTool::Pen);
    draw_pen_down(0.1f, 0.2f);
    CHECK(draw_pen_active() && sent.size() == 1 && sent[0].count == 1);
    const uint32_t s1 = sent[0].stroke;
    for (int i = 1; i <= 130; ++i) draw_pen_move(0.1f + i * 0.004f, 0.2f + i * 0.002f);
    draw_pen_up();
    CHECK(!draw_pen_active());
    {
        const DrawStroke* s = stroke_of(t_self, s1);
        CHECK(s && s->layer == kDrawLayerMap && s->pts.size() == 131);
        const auto w = wire_points(s1);
        CHECK(w.size() == 131);
        for (size_t i = 0; i < w.size(); ++i) CHECK(w[i].x == s->pts[i].x && w[i].y == s->pts[i].y);
        for (const auto& m : sent) CHECK(m.count <= kDrawChunk && m.layer == kDrawLayerMap && m.scope == 0);
        CHECK(s->pts[0].x == (uint16_t)(0.1f * 65535 + 0.5f));
    }

    // --- another player's lines ---
    draw_on_message(0, points(900, kDrawLayerMap, 0, { {100, 100}, {200, 200} }));
    draw_on_message(0, points(900, kDrawLayerMap, 0, { {300, 300} }));
    CHECK(stroke_of(0, 900) && stroke_of(0, 900)->pts.size() == 3);
    draw_on_message(t_self, points(901, kDrawLayerMap, 0, { {1, 1} }));            // our own echo is never taken
    CHECK(!stroke_of(t_self, 901));
    draw_on_message(9, points(902, kDrawLayerMap, 0, { {1, 1} }));                 // nobody's id
    CHECK(!stroke_of(9, 902));

    // --- the eraser takes out what is inside its circle, of this player's lines only ---
    sent.clear();
    // the other player's stroke lies at (100..300)/65535 -- right under this point; ours runs from (0.1, 0.2)
    CHECK(draw_erase_at(300 / 65535.0f, 300 / 65535.0f, 20) == 0 && stroke_of(0, 900) && stroke_of(0, 900)->pts.size() == 3);
    CHECK(draw_erase_at(0.9f, 0.9f, 20) == 0 && sent.empty());                      // nothing of ours there: nothing sent
    {
        // a twin of our stroke held by another player, to see that the message cuts it exactly as it cut ours
        const std::vector<DrawPt> w = stroke_of(t_self, s1)->pts;   // the stroke as every screen holds it
        DrawMsg twin = points(7000, kDrawLayerMap, 0, {});
        for (size_t i = 0; i < w.size();) { twin.count = 0; while (twin.count < kDrawChunk && i < w.size()) twin.pts[twin.count++] = w[i++]; draw_on_message(0, twin); }
        sent.clear();
        const double cx = (0.1 + 50 * 0.004) * 1920, cy = (0.2 + 50 * 0.002 + 0.005) * 1080;    // 5 stage px off the line, mid-way
        CHECK(draw_erase_at(0.1f + 50 * 0.004f, 0.2f + 50 * 0.002f + 0.005f, 20) == 1);
        const DrawStroke* s = stroke_of(t_self, s1);
        CHECK(s && runs_of(*s) == 2);                                                  // cut in two
        CHECK(nearest(*s, cx, cy) >= 20 - 0.1);                                        // nothing left inside the circle
        CHECK(s->pts.front().x == w.front().x && s->pts.back().x == w.back().x);       // the far ends untouched
        CHECK(s->pts.size() < w.size() && s->pts.size() > w.size() - 40);              // only the middle went
        CHECK(sent.size() == 1 && sent[0].kind == kDrawEraseAt && sent[0].count == 1 && sent[0].radius == 320 && sent[0].layer == kDrawLayerMap);
        draw_on_message(0, sent[0]);                                                   // the same circle, over the twin
        const DrawStroke* t = stroke_of(0, 7000);
        CHECK(t && t->pts.size() == s->pts.size() && t->cut == s->cut);
        for (size_t i = 0; i < t->pts.size(); ++i) CHECK(t->pts[i].x == s->pts[i].x && t->pts[i].y == s->pts[i].y);
        // the eraser over the start trims it; over all of it takes the stroke away
        sent.clear();
        CHECK(draw_erase_at(0.1f, 0.2f, 20) == 1 && stroke_of(t_self, s1) && stroke_of(t_self, s1)->pts.front().x != w.front().x);
        CHECK(nearest(*stroke_of(t_self, s1), 0.1 * 1920, 0.2 * 1080) >= 20 - 0.1);
        draw_on_message(0, sent[0]);
        CHECK(stroke_of(0, 7000)->pts.size() == stroke_of(t_self, s1)->pts.size());
        CHECK(draw_erase_at(0.4f, 0.4f, 2000) == 1 && !stroke_of(t_self, s1));
        draw_on_message(0, sent.back());
        CHECK(!stroke_of(0, 7000));
    }

    // --- the other player erases and clears ---
    draw_on_message(0, points(903, kDrawLayerMap, 0, { {5, 5} }));
    DrawMsg er; er.kind = kDrawErase; er.stroke = 900; draw_on_message(0, er);
    CHECK(!stroke_of(0, 900) && stroke_of(0, 903));
    draw_on_message(0, points(904, kDrawLayerBattle, 0xB2, { {5, 5} }));
    DrawMsg cl; cl.kind = kDrawClear; cl.layer = kDrawLayerMap; draw_on_message(0, cl);
    CHECK(!stroke_of(0, 903) && stroke_of(0, 904));                                // the battle line stays

    // --- the bin: this player's lines on what shows, nobody else's ---
    draw_on_message(0, points(905, kDrawLayerMap, 0, { {5, 5} }));
    draw_pen_down(0.5f, 0.5f); draw_pen_move(0.6f, 0.6f); draw_pen_up();
    t_battle = true; t_battle_id = 0xB2;
    draw_pen_down(0.3f, 0.3f); draw_pen_up();                                       // a battle line of ours
    t_battle = false;
    sent.clear();
    draw_clear_own();                                                               // on the map
    CHECK(sent.size() == 1 && sent[0].kind == kDrawClear && sent[0].layer == kDrawLayerMap);
    CHECK(stroke_of(0, 905) && count_of(t_self) == 1);                             // our battle line is left

    // --- a new battle lets the old one's lines go; leaving the run lets everything go ---
    t_battle = true; t_battle_id = 0xB2; draw_tick();
    CHECK(stroke_of(0, 904) && count_of(t_self) == 1);
    draw_on_message(0, points(906, kDrawLayerBattle, 0xB3, { {5, 5} }));         // the next battle's, early
    t_battle_id = 0xB3; draw_tick();
    CHECK(!stroke_of(0, 904) && count_of(t_self) == 0 && stroke_of(0, 906) && stroke_of(0, 905));
    t_battle = false;

    // the pen lifts by itself when the map is covered
    draw_pen_down(0.4f, 0.4f); CHECK(draw_pen_active());
    live({ "Map_Backpack", "Shop_ExitButton" }); draw_tick(); CHECK(!draw_pen_active());
    live({ "Map_Backpack" });

    t_page = PageState::House; draw_tick();
    CHECK(draw_stroke_count() == 0 && draw_tool() == DrawTool::None);
    t_page = PageState::InGame;

    // --- a flood is bounded ---
    for (uint32_t i = 0; i < (uint32_t)kDrawMaxStrokes + 50; ++i) draw_on_message(0, points(2000 + i, kDrawLayerMap, 0, { {1, 1} }));
    CHECK(draw_stroke_count() == kDrawMaxStrokes && !stroke_of(0, 2000) && stroke_of(0, 2000 + kDrawMaxStrokes + 49));
    {
        DrawMsg big = points(5000, kDrawLayerMap, 0, {});
        big.count = kDrawChunk; for (uint32_t i = 0; i < kDrawChunk; ++i) big.pts[i] = { 7, 7 };
        for (int i = 0; i < 200; ++i) draw_on_message(0, big);
        CHECK(stroke_of(0, 5000) && stroke_of(0, 5000)->pts.size() == kDrawMaxPoints);
    }

    printf("draw: %u checks passed\n", checks);
    return 0;
}
