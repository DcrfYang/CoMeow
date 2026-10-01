// mgmp_abandon.h -- "Abandon Adventure" is a vote (proto 48, 2026-09-30).
//
// The pause menu's fifth button ends the run for the player who pressed it. In a room that would end
// it for one player and strand the others in a run that no longer has a party, so the press becomes a
// PROPOSAL: everyone else is asked, the proposer waits, and only when every player has agreed does
// every peer run the game's own abandon, all at once. Anyone refusing puts everything back.
//
// HOW THE CLICK IS CAUGHT. Button_PauseMenu_GiveUp's callback is PauseMenu::TryAbandonRun, which does
// nothing but build the confirmation popup; T_TryAbandon detours it. With no session (or one player)
// the original runs untouched. In a session the detour swallows the call and this module takes over --
// the popup never opens, the vote dialog is the confirmation.
//
// HOW IT IS EXECUTED. The popup's OK callback is one call, start_transition(MewDirector, "defeat",
// <std::function calling the end-of-run>, 0.0), plus tidying of the popup that only the pause menu needs.
// mgmp_abandon_game.cpp makes that call directly, so a peer that never opened a pause menu can run it.
//
// NO ARBITER. Every message is authored by the peer it names and relayed by the host (see relayed()), so
// every peer sees the same set and reaches the same verdict by itself:
//   * a proposal is named by (proposer id, nonce); answers carry both;
//   * unanimity = every member of the session at the moment of the proposal has agreed (the proposer
//     counts); one Deny, a Cancel, a timeout or ANY change of membership ends it, on every peer alike;
//   * two proposals at once: the lower proposer id wins, the other is dropped without a word.
#pragma once

#include <cstdint>

namespace mgmp {

struct AbandonMsg;

// The hook (T_TryAbandon). True = the press was taken by the vote and the game's own must not run.
bool abandon_on_try();

void abandon_on_message(uint8_t from, const AbandonMsg& m);

// From the frame hook: timeouts, membership, session going away.
void abandon_tick();
void abandon_reset();

enum class AbandonPhase : uint8_t {
    None,
    Proposing,   // I proposed; waiting for the others
    Asking,      // somebody else proposed; my answer is wanted
    Answered,    // I agreed; waiting for the rest
};

constexpr uint32_t kAbandonMaxPlayers = 4;

struct AbandonView {
    AbandonPhase phase = AbandonPhase::None;
    uint8_t  n = 0;                                  // players, by position in the session (host first)
    uint8_t  self_pos = 0;
    uint8_t  proposer_pos = 0;
    bool     agreed[kAbandonMaxPlayers] = {};
    uint32_t seconds_left = 0;
};
bool abandon_view(AbandonView& out);

// The dialog's buttons.
void abandon_vote(bool agree);       // while Asking
void abandon_withdraw();             // while Proposing

// Why a vote ended, once, for the panel's toast.
enum class AbandonNoticeKind : uint8_t {
    None,
    Denied,          // `pos` refused
    Withdrawn,       // `pos` (the proposer) took it back
    TimedOut,
    Busy,            // the local press met a vote already in progress
    Changed,         // someone joined or left
    Going,           // unanimous: abandoning now
};
struct AbandonNotice { AbandonNoticeKind kind = AbandonNoticeKind::None; uint8_t pos = 0; };
bool abandon_take_notice(AbandonNotice& out);

// --- the game side (mgmp_abandon_game.cpp; the tests stub it) --------------------------------------------
bool abandon_native_ready();    // everything the call needs resolved, and a director to call it on
bool abandon_run_native();      // start the abandon on THIS peer

} // namespace mgmp
