// mgmp_housecoop.h -- EXPERIMENTAL (ui.exp_house_coop, the HOST's setting): helping the one player whose house boss is today (2026-10-18).
//
// Without it a house boss is played alone (mgmp_room: the player marks itself away and leaves the session). With it on, in a room:
//
//   1. THE ASK. Every player says, about once a second, whether it stands in its warehouse and whether its own save has the house boss today
//      (MSG_HOUSECOOP kHcStatus). When EXACTLY ONE player -- PX -- has it today, nobody else has, and everybody is in the warehouse, PX's game
//      asks the others (kHcAsk): every other player gets "help PX with their house defense?". PX waits meanwhile (and may choose to fight alone).
//   2. THE ANSWER. All yes -> PX says go (kHcGo); one no (or a player leaving) -> cancelled (kHcCancel) and PX fights alone as before.
//   3. THE AUTOSAVE. At the go every player of the co-op saves the game and takes an auto save (the backup page's AUTO list) and is told so:
//      what follows leaves a helper's save in a state of its own until the fight is over.
//   4. THE INSERTED DAY. A helper's next end of day (the door with an empty box, the bed ...) is not a day: the house boss countdown, the
//      overnight (food, breeding, the day counter) and the new day's strays are skipped, and PX's boss is put in the helper's House instead --
//      boss name, countdown 0, PX's times fought and difficulty (the fight is built from them, so every screen must have PX's). The helper's own
//      four values are kept (and written to a small file beside the saves, for a game that does not come back).
//   5. THE FIGHT. Every player sets off from its own House the game's own way -- the departure box takes retired cats that have not defended yet
//      on a house boss day, the room's party-size rule applies, there is no collar page -- and the house boss gear screen's done is held
//      (mgmp_setup: the cats are exchanged, every player READY, the host commits the stream and its unlock answers); then every peer starts the
//      fight at once with the same battle id (lockstep) and the same build inputs. The room's enemy rule already doubles every enemy's health.
//   6. THE END. Only PX's settlement runs the story (GlobalProgressionData::BeatHouseBoss); a helper's is skipped and, back home, its own house
//      boss values are put back and saved. Nothing of the inserted day stays in its save.
//
// The sync saves along the way are the room's usual ones: the preparation save while everybody is in the warehouse (now on the boss day), the
// ready save while the gear screens are held, and the auto save at the go; the run journal starts at the commit like any run.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

struct HouseCoopMsg;

void housecoop_tick();                                        // every frame (room_tick)
void housecoop_on_message(uint8_t from, const HouseCoopMsg& m);

// mgmp_room: while true the house boss is NOT started alone -- a vote is open, a co-op is on, or this player has the boss today and the
// others' answer is still awaited.
bool housecoop_holds_solo();

// 0.1.92 diagnostics (read only, log tag HOUSEBOSS): this save's house boss, its rematch list and the co-op's state; and why housecoop_holds_solo()
// holds right now ("" when it does not).
void housecoop_log_boss(const char* why);
void housecoop_hold_reason(char* out, size_t cap);

// --- the hooks (mgmp_hooks) --------------------------------------------------------------------------
bool housecoop_skip_countdown();                              // T_HouseCountdown: true = skip it (an inserted day)
bool housecoop_overnight(void* out_vec);                      // T_HouseOvernight: true = skipped (out_vec emptied, PX's boss put in)
bool housecoop_skip_strays();                                 // T_HouseStrays: true = skip it
bool housecoop_on_fight_start(void* data, void (*original)(void*));   // T_HouseFight: true = held (the caller returns)
bool housecoop_skip_beat_boss();                              // T_BeatHouseBoss: true = skip (a helper)
bool housecoop_refuse_depart(uint32_t cats);                  // T_TryDepart with the cats in the box: true = refused (a helper must end its day first)
void housecoop_settlement_begin();                            // the start of the native end of a run
void housecoop_after_settlement();                            // run_settled, at the end

// --- what the menu draws ------------------------------------------------------------------------------
enum class HcPanel : uint8_t { None, Ask, WaitVotes, WaitHouse, Voted, GearWait };
struct HouseCoopView {
    HcPanel panel = HcPanel::None;
    int     px_row = 0;          // P<px_row>: the helped player
    char    boss[48] = {};
};
bool housecoop_view(HouseCoopView& out);
void housecoop_answer(bool yes);                              // the helper's popup
void housecoop_fight_alone();                                 // PX's "fight alone"

} // namespace mgmp
