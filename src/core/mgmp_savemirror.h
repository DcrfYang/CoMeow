// mgmp_savemirror.h -- an untouched copy of the game's save folders, taken once, at the first launch with the mod.
//
// WHY (2026-10-05): the backup page has been reported to do the wrong thing for some players (a second Steam account's folder, a backup that would not load). Whatever it does, the player's own saves must
// never depend on it. So when the mod starts, the whole of the game's save folder -- <roaming appdata>\Glaiel Games\Mewgenics, every account's folder in it, with whatever the game and the mod have put there
// -- is copied to <the mod's folder>\原存档\ (the contents of that folder go straight into it: 原存档\<account id>\saves\...). If 原存档 already exists nothing is copied: it is the state of the saves BEFORE
// the mod touched them, and a second copy would overwrite exactly that.
//
// The copy is made in 原存档.partial and renamed when it is done, so an 原存档 that exists is a complete one; a launch that was cut short finds the .partial and starts it again. It runs on its own thread
// at low priority, after the hooks are up, and only reads the game's files.
#pragma once

namespace mgmp {

void savemirror_start();   // returns at once; the copy runs on a background thread

} // namespace mgmp
