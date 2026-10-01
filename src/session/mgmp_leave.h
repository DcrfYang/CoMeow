#pragma once
// mgmp_leave -- the host left the run, so the client should not still be in it.
//
// THE GAP THIS CLOSES. Everything the host does INSIDE a run announces itself:
// ENTERNODE at each node, CHOICE at each decision screen, CATDATA / INVENTORY /
// RUNHIST at each boundary, ACTION and HASH at each turn. Leaving the run
// announces nothing -- the host just stops sending, which looks exactly like a
// host who is thinking, reading a tooltip, or three turns into a long battle.
//
// So a host who goes back to the house, abandons the run, or quits out to the
// title screen leaves the client sitting inside a run nobody else is playing,
// and neither log says a word about it. That is the same class of failure as
// the stranded-on-the-main-menu report of 2026-08-28, arrived at from the other
// direction.
//
// --- how "in a run" is decided ----------------------------------------------
//
// From TWO readings, and it takes both. The game's own loaded-scene list says
// OUT when House, MainMenu or SaveSelectionScreen is loaded and not marked for
// destruction (see the kScene_* block in mgmp_addresses.h); savefile's cat-id
// test says whether a run is actually loaded. A peer is IN a run only when the
// scene list does not say OUT *and* the cat-id list says a run exists.
// Anything else is Unknown.
//
// THE POSITIVE HALF IS NOT BELT AND BRACES. Absence of an out-scene is a
// reading nothing has to be true for, and at startup nothing is: the game boots
// through Shared / Base / Tutorial / Cutscene with no MainMenu scene yet, so
// the absence test alone said "in a run", and the moment MainMenu loaded the
// host announced a departure that never happened and took the client out with
// it. Measured 2026-08-28 before a save had even been picked.
//
// The asymmetry is deliberate and survives that correction: Unknown neither
// arms the announcement nor triggers it, so a transient gap during a real
// transition can only ever suppress, never manufacture. Two consecutive OUT
// confirmations are required on top, which is a second of wall clock at the
// poll rate here and costs nothing because leaving a run is not a decision
// anybody takes twice a second.
//
// --- what the client does about it, and the one thing it cannot do ----------
//
// It presses Quit To Menu for the player, through Button::Click, from the same
// Button::update hook that already presses Play -- the proven route, and the
// one that goes through the game's fade and the game's own guards rather than
// synthesising a scene transition over a live battle.
//
// THE PAUSE MENU HAS TO BE OPEN. Button_PauseMenu_QuitToMenu does not exist
// until PauseMenu::init has run and SetupMainSidebar has built the sidebar, and
// PauseMenu::init only runs when a person pauses -- there is no persistent
// PauseMenu component to reach into and no update override to hook (its slots
// 6..15 are the ICF-folded empty virtual, the same dead end MainMenu had). So
// this feature is "press Escape and the mod does the rest", and it says so, at
// a severity the player will actually see. Dragging someone out of a run
// without a keypress would mean building the transition ourselves across a
// scene teardown nobody controls, with the battle layer holding Character*
// pointers into it; that is a worse failure than the one being fixed.
//
// A client that is ALREADY out of a run when the message arrives does nothing
// and says so at Trace. That is the common case when the host is merely
// re-picking a save slot, and it is not a problem to be reported.

#include <cstddef>
#include <cstdint>

namespace mgmp {

struct HostLeftMsg;

void leave_init();
void leave_shutdown();
void leave_set_base(uintptr_t base);

// Every frame, from the session's Ready tick. Polls the scene list on a divisor
// (see kPollFrames) rather than every frame: three guarded reads per loaded
// scene is cheap but not free, and nothing here is urgent to the millisecond.
//
// Runs on BOTH roles. The host watches for its own departure; the client uses
// the same poll to notice that it has arrived back on the menu, so the "still
// trying to leave" state cannot outlive the leaving.
void leave_pump();

void leave_on_message(const HostLeftMsg& m);

// From h_ButtonUpdate, after the original -- same slot and same reasoning as
// savefile_on_button_update. Inert unless this peer is a client that has been
// told the host left and is still inside a run.
void leave_on_button_update(void* button);

// PANEL: arm the leave path as though the host had announced it.
//
// The feature has three stages that fail identically from the outside -- the
// host never announced, the client declined the announcement, or the client
// armed and never saw the button -- and telling them apart from a screenshot is
// impossible. This skips the first two: press it, open the pause menu, and if
// Quit To Menu is not pressed then the fault is the click and nothing else.
//
// Deliberately not role-gated. It is a test of a local mechanism, and refusing
// it on a host would remove the one machine a developer is usually sitting at.
void leave_request_local();

// --- which menu screen is up (2026-09-29) -----------------------------------------
//
// For the player-facing menus (mgmp_menu): "is the title screen showing, and
// nothing else". Same scene walk as everything above, same guarded reads, but it
// works with NO session and NO role -- the menus exist to create one.
//
// MainMenu means the MainMenu scene is live (not marked for destruction) and
// neither the save-selection screen nor a House is. Unknown is a walk that did
// not read as a scene list, or a boot that has not reached the title yet; the
// caller must treat it as "do not draw".
//
// Refreshed every few calls and cached between, for the reason leave_status
// documents: a scene walk during a teardown lands on freed objects, and a menu
// asking sixty times a second was exactly that.
enum class MenuScreen { Unknown, MainMenu, SaveSelect, House, Other };
MenuScreen leave_menu_screen();

// The live scenes' names, comma separated, dying ones left out; false when the list
// does not read. A fresh walk on every call -- for the page-state reader, which
// asks about twice a second, and for logs.
bool leave_scene_summary(char* out, size_t cap);

// Is a scene with this exact name loaded and not being torn down? A FRESH walk on
// every call, unlike leave_in_run / leave_in_house, whose answers come from the
// poll above -- and that poll only advances while a session is Ready.
//
// That distinction is the whole reason this exists. A player can be on the map, or
// in the warehouse, with no room open and no socket up: the mod's own menus are
// usable there, and a panel that reported them as Unknown because a session-gated
// cache had never been filled would be wrong about the one thing it exists to say.
bool leave_scene_live(const char* name);


// --- the co-op session decision (2026-09-22) --------------------------------------
//
// Requirement, in the user's words: when one side stops co-operating, the OTHER side
// is asked, and may continue ALONE with its own cats; and until it answers, the mod
// must not let it play on.
//
// TODAY'S BEHAVIOUR IS THE OPPOSITE, and that is what these replace: a client told
// "the host left" has Quit To Menu PRESSED FOR IT -- it is dragged out of the run.
// The old behaviour is not lost: it is now the WAIT branch. What changes is that it
// stops being the only answer, and that it no longer happens without being asked.
//
// The block is real but partial: an undecided peer presses nothing (so the run is not
// left behind its back), and the panel puts the question up.
//
// --- THE SPEC OF THE HOLD, AS DECIDED (2026-09-22, user) ---------------------------
//
// Read this before touching any of the three below: the requirements are the user's,
// and two of them are narrower than the obvious reading.
//
// 1. THE HOLD BLOCKS ONE THING: ENTERING THE NEXT NODE. Not the pause menu, not the
//    inventory, not a battle already in progress -- those all keep working, and the
//    user said so explicitly ("nothing else is needed"). A peer that can still look at
//    its cats and its map is a peer that can decide; a peer whose whole game is frozen
//    is a bug report.
//
//    Note that this is very nearly free ALREADY: a client's node clicks are swallowed
//    by mgmp_follow (`suppressed local map input`), and with the host gone no node
//    message arrives to move it either. Verify that from the log before writing code
//    for it -- the expected evidence is a `FOLLOW suppressed local map input` line
//    when an undecided client clicks a node.
//
// 2. THE WAREHOUSE IS THE HOUSE SCENE. "The two of you cannot pair again until both
//    are back at a warehouse" -- that warehouse is the House (the room the game builds
//    at MewDirector::init, the one with the cats in it), and mgmp_leave can ALREADY
//    read it: leave_in_house() exists for savefile's walk-out test, and the panel's
//    `leave:` line prints it (observed live: `idle -- House`). So the release point
//    needs no new detection. A save that has not started a battle opens into the
//    House, which is why "in the House" is a state a run can legitimately be in.
//
// 3. ABANDONING THE ADVENTURE IS AN ANSWER TOO. The pause menu's fifth entry -- save
//    and return to the warehouse, i.e. end this adventure by hand -- counts as
//    abandoning the co-op session, and must therefore RELEASE the hold rather than be
//    blocked by it. Its destination is the House, which is exactly the reading in (2),
//    so the release needs no button name, only the arrival: while the decision is
//    pending, arriving in the House means the run was abandoned by hand.
//
// What `Continue alone` must actually DO, once answered: drop the connection (the
// departing peer is gone), rewrite the run's party list to this peer's own cats
// (mgmp_roster already owns that write -- it is the layer that decides who fights),
// and refuse to pair again until the House. The decision itself must also be RESET
// when a new run arrives, or a peer that re-enters without answering keeps a
// question nobody can dismiss.
bool leave_decision_pending();
void leave_decide_continue_alone();
void leave_decide_wait();

// A NEW RUN MAKES THE OLD QUESTION MEANINGLESS (2026-09-22).
//
// Measured in a live session: the peer was told the host had left, the question went up
// and was never answered, and then the host picked a save again. The client dropped the
// old run and followed the new one in -- and the question came with it, still undecided,
// still holding Quit To Menu back. A question about a run that no longer exists cannot be
// answered, so it must not survive one.
//
// Called by mgmp_savefile where a fresh pick is applied (the same place that says "the
// host picked a save again"). Safe to call at any time; it is a no-op unless there is
// something to clear.
void leave_run_reset();
// Native finalization completed. Returning home is not a lost co-op peer.
void leave_on_settlement();

// --- continuing alone (2026-09-22, the user's requirement) -------------------------
//
// "Continue alone" is now a real state rather than a record: the peer stops following,
// the run's party is rewritten to its own cats, and the session is closed. What holds it
// together afterwards is a LATCH -- the user's rule, in their words: the two of you
// cannot pair again until both are back at a warehouse.
//
// THE WAREHOUSE IS THE HOUSE, the same reading the abandon case uses (leave_in_house), so
// the latch clears there and nowhere else.
bool leave_solo_active();

// Asked before a host or join request is accepted. Returns false AND SAYS WHY at Warn
// when the latch is set and this peer is not in the House; `what` names the action for
// the line ("hosting", "joining").
bool leave_pairing_allowed(const char* what);

// PANEL: one line of live state -- armed or not, where this peer thinks it is,
// how many presses are left and whether the cooldown is holding one off.
//
// Every stage of this feature fails invisibly from the outside, and reading it
// back out of the log after the fact has now cost two rounds of "it did not
// work" with no way to say which half. Writing it on the screen while the
// player is looking at the pause menu is the difference between a report and an
// observation. Always NUL-terminates.
void leave_status(char* out, size_t out_size);

// Whether THIS peer is inside an adventure, by the scene test above. Exposed
// because it answers a question several modules currently answer by proxy, and
// because the debug panel should be able to show it.
bool leave_in_run();

// WHICH out-scene, when there is one -- currently only the House is asked about,
// and only by mgmp_savefile: a client that has just loaded the host's run is
// standing in the House (MewDirector::init always ends there) and has to walk out
// of the door before the run exists for it. Same cached poll as above; the point
// of exposing the name rather than a second walk is that "which screen is up" must
// have exactly one implementation to fix when an offset moves.
bool leave_in_house();

// Whether the CHAPTER PAGE is up, decided by its own two buttons ticking rather
// than by a scene name -- see kBtnName_ChapterLower in mgmp_addresses.h for why
// that is the better handle, and leave_on_button for the heartbeat. Fresh within
// half a second; a page nobody is updating is a page nobody is on.
//
// This is the condition the run's barrier will be built on: everything the player
// chooses from here on is shared (the chapter), while everything before it --
// party, gear -- is each peer's own.
bool leave_at_chapter_page();

// Has this peer EVER reached the chapter page? Latched, and asked by two callers
// that must never disagree:
//
//   * mgmp_savefile's first publish -- the save must not go out while the player
//     is still choosing (see the gate in savefile_pump), and
//   * this module's own departure announcement -- a run that was merely LOADED is
//     not a run that can be left, and treating it as one took a session's client
//     out of a run it had just been given.
//
// "Setup done" is the honest name for it on the client too: on a peer that takes
// the host's run, the first screen after the load is where the run it was given
// has begun.
//
// TWO WAYS TO KNOW, AND A RESUMED RUN NEEDS THE SECOND. The chapter page is where
// a NEW run passes; a save whose adventure is already under way loads STRAIGHT
// ONTO THE MAP, with no House and no chapter screen, so a chapter-only test never
// became true and the client waited on the main menu for a save that was never
// sent (measured 2026-09-22). Hence `kScene_Map` beside it: being on the Map scene
// means being inside the run.
bool leave_setup_done();

// Called from the Button::update detour. Cheap: two string compares, and only
// after the caller has established that this frame has a live button.
void leave_on_button(void* button);

} // namespace mgmp
