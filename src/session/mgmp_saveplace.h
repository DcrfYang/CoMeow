// mgmp_saveplace.h -- WHERE A SAVE STANDS (0.1.249, 2026-11-22). Every list of saves the mod shows -- the backup positions, the automatic backups,
// the handshake saves of the backup page and the host's list in a room -- says where loading it puts the player: the warehouse, the gear page, the
// chapter page, or a map and the node the party stands on ("出发点" right after arriving).
//
// WHAT THE SAVE ITSELF SAYS (measured on 1,000+ saves and handshake files, and the game's loader 0x1403BBFF0):
//   properties.on_adventure 0 ........................ the warehouse (the game writes nothing on the collar page: a save taken there is still this one)
//   on_adventure 1, adventure_started 0, no map ...... the gear page (the loader sends it to "adventure_prepare")
//   on_adventure 1, adventure_started 0, chapter_map . the chapter page (the map is drawn when the chapter page comes up; kStageReady saves look like this)
//   on_adventure 1, adventure_started 1 ............... on the map: files.chapter_map names the node the party stands on
// chapter_map (MapScreen serializer 0x140228E00): seed[32], str map file, u32 10, u32, u8, str current node's name, u32 its column, u64 n, then per node
// { str name, u32 column, u8 finished (MapNode+0x168), seed[32], str level }. A str is a u64 length and the bytes. The name is the node's key in the
// map's gon ("battle", "shop_water", "abilityneedle" ...): the map-specific ones are named in mgmp_saveplace.cpp, anything else is shown as unknown
// and logged (SAVEPLACE) so it can be added.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mgmp {

enum class SavePlace : uint8_t { Unknown = 0, House, Collar, Gear, Chapter, Map };

struct SavePlaceInfo {
    SavePlace place = SavePlace::Unknown;
    bool     start = false;     // Map: the party is still on the map's starting point
    bool     done  = true;      // Map: the node it stands on is finished (false: the game was quit inside it, a battle's pause menu)
    char     map[24]   = {};    // Map / Chapter: the map file without ".gon" ("sewers")
    char     kind[32]  = {};    // Map: the node's key in the map's gon ("battle", "shop_water")
    char     level[32] = {};    // Map: the node's level ("boris" on a boss node, mostly "unknown")
    char     why[112]  = {};    // Unknown: what could not be read or was not recognised (logged)
};

// The decision itself, from what was read out of the save (no file access: unit-tested). on_adventure / adventure_started < 0 = not in the save.
SavePlaceInfo saveplace_decode(bool readable, int64_t on_adventure, int64_t adventure_started, const std::vector<uint8_t>* chapter_map);
// Reads `db` (a game save file). `cached`: keep the answer for this path while its size and write time stay the same (a probe file the caller
// rewrites with other saves must pass false).
SavePlaceInfo saveplace_read(const std::wstring& db, bool cached = true);
// The player-facing words: "仓库", "选择装备", "章节选择", "下水道 · 普通战斗", "实验室 · 出发点", "下水道 · 普通战斗（未完成）", "未知".
void saveplace_text(const SavePlaceInfo& p, char* out, size_t cap);
// The node's own label alone ("普通战斗"), nullptr when the kind is not known.
const char* saveplace_node_label(const char* kind);

} // namespace mgmp
