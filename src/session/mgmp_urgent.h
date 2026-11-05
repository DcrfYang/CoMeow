#pragma once
// mgmp_urgent -- the server's URGENT FIX notice (2026-11-04).
//
// When an update fixes something players should not keep playing without, the deploy marks it urgent (scratch/deploy_update.py --urgent "text"):
// the download server then serves /update/urgent.txt -- {id, version, text, issued, expires} -- until it expires (12 h by default). Every running
// game asks for it about once a minute (plain HTTP to the official server's address, on a thread of its own); when this mod is OLDER than the
// notice's version, the menu shows a red banner across the top of the screen and a line in the room chat: the fix exists, restart the game before
// the next battle. A newer (or the same) mod shows nothing. The banner can be put away; a newer notice brings it back.
#include <cstdint>

namespace mgmp {

struct UrgentView {
    char version[24] = {};     // the urgent fix's version ("0.1.186")
    char mine[24] = {};        // this mod's version
    char text[600] = {};       // what it fixes, as the deploy wrote it
    char id[64] = {};
};

// Starts the polling thread (once; later calls do nothing). Called from the menu's frame.
void urgent_start();
// True while a notice applies to this mod (older than the fix) and has not been put away by the player.
bool urgent_view(UrgentView& out);
// True once per notice id, the first frame it applies: the caller adds the chat line.
bool urgent_take_new(UrgentView& out);
// The player put the banner away (for this notice id).
void urgent_dismiss();

// THE GAME UPDATE NOTICE (2026-11-05): Mewgenics was updated and the mod does not fit the new version yet (deploy_update.py --game-notice OLD_VERSION).
// The same thread asks /update/gamenotice.txt -- {id, old, text, issued} -- and every mod shows it (a red banner on the menu) until the player puts it
// away; a newer notice (another id) brings it back, a notice taken down takes it away.
struct GameNoticeView {
    char id[64] = {};
    char text[1200] = {};
};
bool gamenotice_view(GameNoticeView& out);
void gamenotice_dismiss();

// "a.b.c(.d)" compared numerically: <0, 0, >0. Exposed for the tests.
int urgent_version_cmp(const char* a, const char* b);

} // namespace mgmp
