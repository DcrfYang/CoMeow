// mgmp_draw.h -- the drawing board (proto 87): lines a player draws over the map or the battle, shown on every screen in the player's
// colour (P1..P4 of the multiplayer panel), the way Slay the Spire 2's co-op does. Three tools sit in the bottom-right corner: the pen,
// the eraser (one of this player's own lines at a time) and the bin (all of this player's lines on what is showing).
//
// TWO LAYERS. A line drawn on the map belongs to the map and is shown only while the map itself is showing -- not over the backpack, an
// event, a shop or the pause menu, and not in a battle. A line drawn in a battle belongs to that battle (its battle id) and goes with it.
// Leaving the run takes the map's lines with it.
//
// This module is the data and the wire: the strokes, the tool, which layer is showing and the messages. The drawing and the mouse live
// in mgmp_menu (draw_board). Nothing here touches the game or the simulation: a line is something shown, never an action.
#pragma once

#include <cstdint>
#include <vector>
#include "mgmp_proto.h"

namespace mgmp {

enum class DrawTool : uint8_t { None = 0, Pen, Eraser };

constexpr int      kDrawMaxStrokes = 600;      // all players' strokes kept at once; the oldest goes first
constexpr uint32_t kDrawMaxPoints  = 4000;     // points in one stroke; the rest of a longer one is not kept
constexpr unsigned kDrawFlushMs    = 50;       // a stroke being drawn is sent at least this often

struct DrawStroke {
    uint8_t  owner = 0;                        // the author's session id
    uint32_t id = 0;
    uint8_t  layer = kDrawLayerMap;
    uint64_t scope = 0;                        // the battle id of a battle stroke, 0 on the map
    std::vector<DrawPt> pts;
    std::vector<uint8_t> cut;                  // per point: 1 = no line from the point before (the eraser went through there)
};

// A session with at least one other player in it.
bool draw_available();
// What is showing: kDrawLayerMap, kDrawLayerBattle, or -1 for neither (a menu, the backpack, an event, a shop ...). `scope` gets the
// battle id for a battle.
int  draw_visible_layer(uint64_t* scope = nullptr);

DrawTool draw_tool();
void     draw_set_tool(DrawTool t);

// This player's pen, in fractions (0..1) of the game's 16:9 picture. down starts a stroke on the visible layer, move adds a point,
// up ends it (and sends what is left of it).
void draw_pen_down(float x, float y);
void draw_pen_move(float x, float y);
void draw_pen_up();
bool draw_pen_active();
// The eraser: what lies within `radius` of (x, y) is taken out of this player's own strokes on the visible layer -- only that part,
// a line through the circle is cut in two -- here and on every other screen. The distance is measured in the 1920x1080 stage, so
// the circle is round whatever the screen. Returns how many strokes it changed (a stroke left with nothing goes).
int  draw_erase_at(float x, float y, float radius_stage);
// The bin: every one of this player's strokes on the visible layer.
void draw_clear_own();

// Once a frame: sends the pending points of the stroke being drawn, and lets go of what no longer belongs (a battle that ended, a run
// that was left).
void draw_tick();
void draw_on_message(uint8_t from, const DrawMsg& m);
void draw_reset();

int  draw_stroke_count();
const DrawStroke& draw_stroke(int index);

} // namespace mgmp
