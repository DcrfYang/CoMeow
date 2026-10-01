// mgmp_debug.h -- debug mode: the switch that makes an instance a test rig.
//
// WHAT IT IS. One flag, decided at startup, that turns on behaviour a play
// session must never have. Today it has exactly one behaviour: on the way out of
// the process, the save slot named by debug.slot_to is overwritten with a copy
// of debug.slot_from -- so the next launch starts from a known baseline without
// anybody having to remember to prepare it by hand.
//
// WHERE THE FLAG COMES FROM, in order:
//
//   1. mgmp_loader.exe --debug / --no-debug. The loader writes a marker
//      (<dll directory>\mgmp_debug.on) that the DLL reads at init. This is the
//      real launch parameter, and the loader writes it for EVERY launch --
//      including --attach, and including the case where no flag was given, in
//      which case any stale marker is DELETED. A flag that outlived the launch
//      that set it would make debug mode a property of the directory rather
//      than of the command line, which is the class of stale state this project
//      keeps paying for.
//   2. mgmp.json's existing debug block, `"debug_mode": true`. What a bare
//      launch -- or a script that knows nothing about --debug -- uses.
//
// WHICH SLOTS. debug.slot_from / debug.slot_to, filenames inside the game's own
// save directory. The game's slot N is steamcampaignNN.sav, and the copy is a
// plain FILE copy: a .sav is a self-contained SQLite database with no slot
// identity inside it (measured 2026-09-21 -- neither file contains its own
// name), so the filename is the whole of the identity and a copy is exact.
//
// WHEN IT RUNS. DllMain's DLL_PROCESS_DETACH, which is the path this mod
// already uses for "the game is closing". By then the game has written its own
// saves (they happen before ExitProcess; this runs during it), the log is still
// open, and nothing else has been torn down yet. The save directory is resolved
// at INIT and cached, because the resolver calls the shell and the loader lock
// is held here.
//
// BOTH INSTANCES AT ONCE is supported, and is a reasonable thing to want: they
// share one save directory, so they have the same slot 1 by construction and
// the two copies agree about what the result should be. What they do not agree
// about is timing -- so the restore writes a per-PROCESS temporary and promotes
// it with a rename, never with a copy over the top of a file the other instance
// may be reading. If the rename loses the race, the slot keeps its previous
// whole contents and the log says so.
//
// WHAT IT CANNOT DO. A process that is killed -- TerminateProcess from the task
// manager, or a crash -- never reaches DLL_PROCESS_DETACH, so nothing is
// restored. That is inherent to running on the exit path rather than a bug, and
// it is the reason the banner says loudly when debug mode is armed.
#pragma once

namespace mgmp {

// Called once from the init thread, after config_load and log_init, and ALWAYS
// (it is what decides whether the flag is on, and it logs which source decided
// it). Does nothing else when debug mode is off.
void debug_init();

// Called from DllMain's DLL_PROCESS_DETACH, FIRST in that list and before
// log_shutdown -- it is the one thing there that writes to the player's save
// directory, so it needs the log alive to say what it did.
//
// Does nothing unless debug mode is on. Never blocks the exit: a failure is a
// warning and the slot is left as it is.
void debug_shutdown();

} // namespace mgmp
