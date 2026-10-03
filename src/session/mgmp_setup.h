#pragma once

#include <cstdint>

namespace mgmp {

struct SetupMsg;
struct ChapterMsg;
struct ChapterSeedMsg;

void setup_init();
void setup_shutdown();
void setup_reset_run();

// Called when the chapter-page buttons are first observed. A client captures
// its local party and sends it; the host waits for that packet before entering
// the first map node.
void setup_on_chapter_page();
// A player without chapter 2 has NO chapter page: the gear screen's lock starts chapter 1 directly (T_EquipDone). In a room that lock is
// taken over -- a client's means READY, the host's is the chapter-1 choice (it waits for every READY, commits, then goes) -- and the
// game is let through later by setup_tick through `go(director)`. True: taken over, the caller must NOT run the closure.
bool setup_on_equip_done(void* director, void (*go)(void*));
// Every frame: drives a taken-over lock (the export, the host's commit, the client's wait for the host's command).
void setup_tick();
// A taken-over lock is waiting: the player is, for the room, on the chapter page.
bool setup_no_chapter_pending();
// Called at the entry of MapScreen::generate_map on every peer: the stream is set to the seed the host committed for this chapter, so the map's
// node seeds do not depend on how many draws each peer made between the commit and the generation. Once per commit.
void setup_on_generate_map();
// Live, idle map only: paired in-run saves skip the chapter screen entirely.
void setup_on_map(uint64_t checkpoint);
bool setup_runtime_ready();
bool setup_has_shared_roster();
// Local preparation only: never restrict shared runtime event equipment.
bool setup_client_preparing();

// The host may enter a map node only after the client acknowledges its import.
// Always true when the local setup experiment is disabled.
bool setup_host_can_enter();

// Actual chapter commit, before the game's fade callback constructs the map.
// A blocked click is discarded; it must never be replayed after readiness.
bool setup_on_select_act(int act);
// Also freezes host difficulty during an already committed chapter fade.
bool setup_client_controls_locked();
// Called only with a currently ticking, validated chapter difficulty control.
void setup_on_chapter_control(void* screen, int act, int32_t* difficulty,
                              void (*select_act)(void*, int));
void setup_on_chapter_message(const ChapterMsg& m, uint8_t from);
void setup_on_chapterseed_message(const ChapterSeedMsg& m, uint8_t from);

// Called by the Ready-path message pump.
void setup_on_message(const SetupMsg& m, uint8_t from);

} // namespace mgmp
