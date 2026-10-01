#pragma once
#include "mgmp_mem.h"
#include <cstdint>
namespace mgmp {
using EquipmentLookup = void* (__fastcall*)(void*, uint64_t);
using EquipmentLocation = int64_t (__fastcall*)(void*, uint64_t);
using EquipmentLegacy = bool (__fastcall*)(void*);
using EquipmentRefresh = void (__fastcall*)(void*);
enum class EquipmentGate { Allow, MainStory, Unreadable };
// A CatData carries its five equipment slots EMBEDDED: Equipment objects of 0x60 bytes from +0x9B0 (the click
// handler 34EB20 addresses them as cat+0x9B0 + 0x60*kind), each with its gon name as a std::string at +8 and
// nothing at all in an empty slot. 2DE0A0 and 2DDFE0 take such an object and look the name up.
constexpr uintptr_t kCat_Equipment = 0x9B0;
constexpr uintptr_t kEquip_Stride  = 0x60;
constexpr uintptr_t kEquip_Name    = 8;
constexpr unsigned  kEquip_Slots   = 5;
// RVA 34EB20 is shared by InventoryButton_* native click callbacks.
// self+38 = screen; self+58 = item UID. Location -1 means warehouse;
// nonnegative locations identify an equipped cat and take the unequip path.
// RVA 2DE0A0 reads quest_item && legacy_quest through the native GON lookup.
inline EquipmentGate equipment_gate(void* self, bool client_preparing,
    EquipmentLookup lookup, EquipmentLocation location, EquipmentLegacy legacy,
    EquipmentRefresh refresh) {
    if(!client_preparing) return EquipmentGate::Allow;
    void* screen=nullptr; uint64_t id=0;
    if(!self||!lookup||!location||!legacy||!refresh||
       !mem_read((uint8_t*)self+0x38,&screen,sizeof(screen))||!screen||
       !mem_read((uint8_t*)self+0x58,&id,sizeof(id)))return EquipmentGate::Unreadable;
    refresh(screen);
    void* item=lookup(screen,id);
    if(!item)return EquipmentGate::Unreadable;
    if(!legacy(item))return EquipmentGate::Allow;
    const int64_t where=location(screen,id);
    if(where>=0)return EquipmentGate::Allow; // keep native unequip available
    return where==-1?EquipmentGate::MainStory:EquipmentGate::Unreadable;
}

// WHAT A CAT ALREADY WEARS (2026-10-01). The click gate above stops a client equipping a main-story item from
// the warehouse, but a cat that is ALREADY wearing one (the quest items stay on the cat between adventures --
// PutridLeech is cursed and cannot even be taken off) would be cloned into the run with it. This asks the game's
// own predicate about each occupied slot. Returns true and the item's gon name for the first main-story item;
// false for none or when the cat cannot be read (the caller decides what unreadable means). An empty slot is
// never passed to the game: its name is empty and a lookup of "" is not something to ask a gon tree.
inline bool cat_wears_story_item(const void* cat, EquipmentLegacy legacy, char* item, size_t cap) {
    if (!cat || !legacy) return false;
    for (unsigned k = 0; k < kEquip_Slots; ++k) {
        const uint8_t* slot = (const uint8_t*)cat + kCat_Equipment + k * kEquip_Stride;
        uint64_t len = 0;
        if (!mem_read(slot + kEquip_Name + 0x10, &len, sizeof(len)) || !len || len > 64) continue;
        if (!legacy((void*)slot)) continue;
        if (item && cap && !mem_read_std_string(slot + kEquip_Name, item, cap)) { item[0] = '?'; if (cap > 1) item[1] = 0; }
        return true;
    }
    return false;
}
} // namespace mgmp
