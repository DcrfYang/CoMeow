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
