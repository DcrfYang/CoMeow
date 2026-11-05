// mgmp_unitprobe.h -- DEV: what a battle unit is made of, read live (2026-11-03, the research before the sync upgrade).
//
// A unit (glaiel::Character) is an ECS entity: Character+0x18 is its Entity, whose component vector (+0x20 capacity, +0x24 count, +0x28 data;
// Entity::GetComponents 0x140969F90) holds every status and passive the unit carries as a component of its own class (a status is created
// by 0x140483120, the factory the console's `status NAME N` calls, which first looks for a live component of the same type id on the entity).
// This probe logs, per unit: each live component's RTTI class and type id with its first fields, each ability (move / attack / bonus / spells)
// with its first fields, and a cat's five item slots -- the raw material for choosing what the sync hashes and repairs.
#pragma once
#include <cstdint>

namespace mgmp {

// The RTTI class name of a polymorphic game object (".?AVPoison@glaiel@@" -> "Poison"); false when the object has no readable RTTI.
bool unit_rtti_name(const void* obj, char* out, unsigned cap);

// Logs one unit (index = its roster index, for the log). `deep` adds the raw field dumps.
void unitprobe_log(uint32_t index, const void* chr, bool deep);

} // namespace mgmp
