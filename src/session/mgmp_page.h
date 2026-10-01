// mgmp_page.h -- where each player is. The question the multiplayer panel used to
// answer with "已连接": connected says nothing about whether the other person is
// sitting in the warehouse, picking a collar, or mid-battle.
//
// WHAT COUNTS AS A PAGE
//
// The player-facing ones, in the order the game puts them in:
//
//   MainMenu      the title screen
//   SaveSlots     the save slots ("开始游戏" leads here)
//   House         the warehouse (the mod calls it the House because the game does)
//   Collar        picking a collar for a cat
//   Equipment     picking gear
//   Chapter       the chapter page -- the last screen before a run begins
//   Map           the map, i.e. a run in progress, from the first node to settlement
//
// HOW IT IS READ, AND WHY NOT BY SCENE NAME
//
// The scene list gives three of them directly (MainMenu, SaveSelectionScreen,
// House, and "Map" for a run in progress), and mgmp_leave already walks it. It
// cannot see the three screens BETWEEN the House and the chapter page, because
// they are not scenes -- they are panels inside the House's scene.
//
// What identifies those is the same thing that identifies the chapter page in
// mgmp_leave: WHICH NAMED BUTTONS ARE TICKING. A button exists while its screen is
// up and is destroyed with it, so "the collar buttons are ticking" is a reading of
// the screen rather than of a remembered flag. The names come from
// mgmp_addresses.h's walk-through of the setup flow (2026-09-21) and from the
// game's own logs, and the two that matter here are:
//
//   CatSelector_Left / CatSelector_Right     the party picker, with the gear
//                                            buttons under the same cat
//   InventoryButton_Collar                   the collar slot specifically
//   InventoryButton_{trinket,weapon,...}     the other gear slots
//   Button_LowerDifficulty                   the chapter page (see mgmp_leave)
//
// The House's own buttons (EndDay_Sign, HouseButton) are deliberately NOT part of
// the test: they tick under the collar and gear panels too, so they say "in the
// House" and nothing finer. The order of the tests below is the resolution.
#pragma once

#include <cstdint>

namespace mgmp {

enum class PageState : uint8_t {
    Unknown = 0,   // this peer cannot tell, or has not looked yet
    MainMenu,
    SaveSlots,
    House,
    Collar,
    Equipment,
    Chapter,
    InGame,
};

// The page as of the last poll, refreshed on a divisor. Safe to call every frame.
PageState page_self();
const char* page_name(PageState p);

// Called from the Button::update detour -- it is where the named screens can be
// seen at all. Cheap: a name read and a few compares, and only for buttons whose
// name starts with a prefix this module cares about.
void page_on_button(void* button);

// Called once per Ready tick, after the button detour has run for the frame.
void page_tick();
void page_reset();

// --- what the OTHER peers say -------------------------------------------------
//
// The same reading, published to everyone else and kept per peer. A peer that has
// said nothing is Unknown, never "main menu" -- see rule 36 in the design notes.
PageState page_of(uint8_t peer);
// Milliseconds since that peer's last page message; 0 if it has never sent one.
uint32_t  page_age_ms(uint8_t peer);

void page_on_message(uint8_t from, uint8_t page);

} // namespace mgmp
