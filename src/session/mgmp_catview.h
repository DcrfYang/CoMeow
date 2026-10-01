#pragma once
// mgmp_catview -- which cats each player is bringing, for the room panel.
//
// The panel already says WHERE everyone is (mgmp_page); this says WITH WHOM: for a
// player in the collar, gear or chapter screens or inside a run, up to four cat cards
// -- head, health, level, class -- to the right of their name.
//
// It is the same shape as mgmp_page and for the same reasons: read locally on a
// timer from the game's own state, sent on a timer, relayed by the host, stored per
// peer id, and never branched on. A peer that has said nothing has NO cards, not a
// card that says "nothing" -- rule 36.
//
// WHAT IS READ is the local party (director+1468) through the game's serializer
// (mgmp_catsync's serialize_cat) and mgmp_catblob.h, filtered by
// lockstep_cat_is_mine, whose contract is that "unknown" keeps the cat: before any
// owner is known every cat in the party is this player's.

#include <cstdint>

#include "mgmp_catbrief.h"
#include "mgmp_page.h"

namespace mgmp {

struct CatsMsg;

// Pump: reads (about once a second) and publishes. Called from FrameBegin, like
// page_tick, because a room can exist before any session does.
void catview_tick();

void catview_on_message(uint8_t from, const CatsMsg& m);
void catview_reset();

// Whether a player standing on this page has cards worth drawing (the two gear pages, the
// chapter page, a run). The panel asks this of the PAGE it holds for that player, so a card
// from a moment ago never outlives the screen it belonged to.
bool catview_page_shows(uint8_t page);

// The cards of this peer / of another peer. Both return how many; `out` points at
// storage that stays valid until the next tick or message.
uint32_t catview_self(const CatBrief** out);
uint32_t catview_of(uint8_t peer, const CatBrief** out);

} // namespace mgmp
