// mgmp_gameconsole.h -- is the GAME'S OWN debug console (glaiel::DebugConsole, enable_debugconsole in settings.txt) open right now? (0.1.150)
//
// The console takes the keyboard like a text box does: while it is open Enter runs the command typed, and Esc / the backquote close it. The mod reads Enter in the
// window procedure before the game does (the room chat, mgmp_ui) and could not tell, so in a room the first Enter of a command opened the chat and the game never got it.
// The console's event handler (0x140A139A0, T_ConsoleEvent) is hooked only to remember WHICH object the console is; its two flags are read where they are:
//   +0x174  the console is enabled (DebugConsole::init sets it when settings.txt says enable_debugconsole true; the handler does nothing otherwise)
//   +0x198  the console is open (set by the toggle key, cleared by it, by Esc, and by an Enter on an empty line)
// The object lives as long as the DebugOverlay scene, which is the whole process.
#pragma once
#include <atomic>
#include <cstdint>
#include "mgmp_mem.h"

namespace mgmp {

constexpr uintptr_t kConsole_Enabled = 0x174, kConsole_Open = 0x198;

inline std::atomic<const void*>& gameconsole_slot() { static std::atomic<const void*> p{nullptr}; return p; }
// The console's event handler ran for this object.
inline void gameconsole_note(const void* console) { gameconsole_slot().store(console, std::memory_order_relaxed); }
// The console exists, is enabled, and is open. False when it was never seen, is disabled, or its memory cannot be read.
inline bool gameconsole_open() {
    const void* c = gameconsole_slot().load(std::memory_order_relaxed);
    uint8_t enabled = 0, open = 0;
    if (!c || !mem_read((const uint8_t*)c + kConsole_Enabled, &enabled, 1) || !mem_read((const uint8_t*)c + kConsole_Open, &open, 1)) return false;
    return enabled == 1 && open == 1;
}

} // namespace mgmp
