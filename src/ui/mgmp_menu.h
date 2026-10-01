// mgmp_menu.h -- the mod's player-facing menus. Layer 5, and NOT diagnostics.
//
// The ImGui debug panel (mgmp_ui.h) is for whoever is reading the log. This is
// for whoever is playing, and it lives in the same ImGui frame only because that
// frame is the one place already drawing after the game and before the present.
//
// WHAT IT DRAWS
//
//   * On the title screen, two more entries under the game's four, in the game's
//     own menu font: "Multiplayer" and "Save backup".
//   * The Multiplayer window: connect to the signaling server, list rooms,
//     create or join one. A room being open IS multiplayer mode; there is no
//     other switch.
//   * The Save backup window: the twenty-position list of mgmp_saveslots.
//   * While a room is open, on every screen: a small paper tag at the right edge
//     that folds a panel out to the left with the players and their state.
//
// WHY IT LOOKS LIKE THE GAME
//
// All the art is the game's own, exported from its SWFs by
// tools/export_ui_assets.py into assets/ next to the DLL: the torn-paper tag of
// the house screen's left column, the confirmation box's paper, the menu font
// (TikaFontCN) and the cursor. Nothing here is drawn from scratch except the
// ink outlines, which are jittered on purpose to sit next to hand-cut paper.
//
// A missing asset is never fatal: every texture falls back to a flat drawn
// shape and the font falls back to the system's CJK face, and the log says which.
#pragma once

namespace mgmp {

// After the ImGui context and both backends are up, before the first frame.
// Loads the font; textures are loaded lazily on first use.
void menu_bring_up();

// Once per frame, inside NewFrame/Render. Cheap when nothing is showing.
void menu_draw();

// From the game's Button::update detour, once per button per frame. Lets the
// title entries follow the native ones' lifecycle: a native entry that stops
// ticking (a modal sits over the menu, the scene is fading out) is one ours
// must stop answering too.
void menu_on_button(void* button);

// F2: opens/closes the "upload the game log" panel (any screen, any role).
void menu_toggle_log_window();

// Ask for the named button to be pressed the next time it ticks (once). For the dev
// harness; the title screen's own auto-Play uses the same path.
void menu_request_press(const char* button_name);

// Dev harness: pick save slot N (0-based) the next time the save screen ticks. -1 if none pending.
void menu_request_slot(int slot);
int  menu_take_slot_request();

} // namespace mgmp
