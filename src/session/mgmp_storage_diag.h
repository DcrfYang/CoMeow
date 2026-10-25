// Targeted, read-only diagnostics for the House storage and its save property.
#pragma once

#include <cstdint>

namespace mgmp {

// The numeric value is PageState from mgmp_page.h. Keeping this header
// independent avoids a page/diagnostic include cycle.
void storage_diag_page(uint8_t page);
void storage_diag_save_loaded();
// 0.1.55 (HOUSE): this player's warehouse money and stash -- the House in memory and the save's properties -- at a named moment; a change since the
// last line is called out.
void storage_diag_house(const char* why);

// Called by the existing property hooks. These functions only record values;
// they never replace a getter result or suppress a setter.
void storage_diag_property_get(const char* key, int64_t local, int64_t answer,
                               bool overridden);
void storage_diag_property_set(const char* key, int value);

// WHERE ITEMS COME FROM (0.1.52). Logged only.
//  * ITEMGET: every Inventory::insert_item -- the item, the place (backpack / storage ...), the caller chain named where it is known (a chest, a shop,
//    a battle reward, an event, an unequip ...), and the node, page and battle it happened in. `before` reads the item (the call may move it), `after` logs.
//  * ITEMS: the backpack, storage and trash compared by NAME with their last reading -- at every page change, save load and node entry -- so a change
//    that does not go through insert_item (a consumable used, an item sold or dropped, an item moved between the three) is still seen.
struct ItemInsertNote {
    bool     on = false;
    char     name[64] = {};
    char     source[420] = {};
    int32_t  loc = 0;
    int64_t  arg = 0;
    uint32_t flags = 0;
    int32_t  durability = 0;
    bool     mine = false;          // the director's own inventory (not a shop's, a chest's ...)
    void*    inv = nullptr;
    uint32_t before[3] = {};        // backpack, storage, trash counts before the call
};
void storage_diag_item_insert_before(ItemInsertNote& note, void* inv, void* item, int32_t loc, int64_t arg, void* ret, void* const* frames, unsigned nframes);
void storage_diag_item_insert_after(const ItemInsertNote& note);
// `full`: also list the whole backpack when it changed (and always after a load).
void storage_diag_items(const char* why, bool full);

} // namespace mgmp
