#pragma once
// THE BOARD'S GROUND TILES (0.1.195, proto 116; room USKT 11-06 00:35, battle a28e72e8, turn 32: the host's tile (2,0) was a WaterTile and the client's a BlankTile -- a unit
// standing there had other elements on each game, and nothing put the tile right: the battle halted).
//
// Every board cell holds one ground object (layer 0 of the cell's list: Board+0xB0, Board+0xB8 wide, Board+0xBC high, 16 bytes a cell {cap, count, data}; each entry is the
// object's position component: +0x70 its layer, +0xA0 the object). Its class is the tile (BlankTile, WaterTile, GrassTile, FireTile, a trap ...), its +0x130/+0x134 the
// elements it gives a unit standing on it. The game changes a tile with a method of the OLD tile object: sub_14089A700(tile, std::string name BY VALUE, bool checked) makes the
// tile of that class on the same cell and retires the old one (TileTrail's own call, 0x1407D3B64; ChangeTile and the rest go the same way).
//
// The host sends its whole ground map at every turn boundary (MSG_TILES: a table of class names and one entry per cell); a client compares its own before the turn is hashed
// and changes every cell whose class differs from the host's, the game's way. The tiles are not in the turn hash: the units' elements (which are) show what matters of them.
#include <cstdint>

namespace mgmp {

struct TilesMsg;

// GAME THREAD. The ground map of `board` (Character+0x80). False when the board is unreadable or larger than the message holds.
bool tiles_read(const void* board, TilesMsg& out);

// GAME THREAD, at a turn boundary, after the host's unit board (mgmp_lockstep). The host sends its map; a client takes the host's map of this boundary (waiting briefly)
// and puts every differing cell right. `board` may be null (no unit to read it from): nothing is done.
void tilesync_boundary(uint64_t battle, uint32_t turn, const void* board, bool host);

// DEV (harness `usdev tile X Y NAME` / `usdev tiledump`): change one cell on THIS game only / log the whole map. Game thread.
bool tilesync_dev_set(const void* board, uint32_t x, uint32_t y, const char* name);
void tilesync_dev_dump(const void* board);

} // namespace mgmp
