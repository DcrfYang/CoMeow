// mgmp_leave.cpp -- see mgmp_leave.h.

#include "mgmp_leave.h"
#include "mgmp_net.h"
#include "mgmp_lockstep.h"   // lockstep_in_battle -- a marker on the press, not a gate
#include "mgmp_savefile.h"   // savefile_adventure_is_loaded -- the second opinion
#include "mgmp_checkpoint.h"
#include "mgmp_setup.h"
#include "mgmp_follow.h"     // follow_shutdown -- "continue alone" ends the following
#include "mgmp_session.h"    // session_request_disconnect -- the teardown that also
                             // shuts the co-op modules down; net_shutdown alone does not
#include "mgmp_config.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"

#include <cstring>
#include <cstdio>

namespace mgmp {
namespace {

// A run has a handful of scenes live at once. The cap is generous and bounded:
// a five-digit count means the offsets moved, and this module must then report
// that rather than walk a megabyte of heap.
constexpr uint32_t kMaxScenes = 64;

// Frames between polls. The scene list changes when a person clicks something,
// so twice a second is already far finer than the event it watches, and it
// keeps the per-frame cost of this module at one increment and one compare.
constexpr uint32_t kPollFrames = 30;

// How many consecutive OUT polls before the host announces. See the header:
// the asymmetry means a transition gap can only suppress, never invent -- this
// is belt and braces on top of that, and it costs one second.
constexpr uint32_t kConfirmPolls = 2;

// The same shape as savefile's auto-Play, for the same reasons: the first press
// is immediate, and if the button is still ticking kRetryFrames later then the
// transition did not start.
constexpr uint32_t kRetryFrames = 120;   // ~2 s at 60 Hz
constexpr uint32_t kMaxPresses  = 5;

// Consecutive failed scene reads before the module says it is blind. Three polls
// at kPollFrames each, i.e. about 1.5 s -- long enough that the empty vector of
// the first frame after boot cannot trigger it, short enough that a real offset
// drift still reports itself while a player is watching. See report_broken.
constexpr uint32_t kBrokenPolls = 3;

// How stale a chapter button's last update may be, in FRAMES. See leave_on_button:
// the page is identified by its buttons ticking, so "no tick recently" is how the
// page going away is noticed. Frames rather than wall clock for the reason the
// rest of this file counts frames -- and because a frame here is the unit the
// button detour itself runs in.
constexpr uint32_t kChapterFrames = 30;   // ~0.5 s at 60 Hz

// WHERE THIS PEER IS, from two independent readings, because either can drift.
//
// The scene walk answers "which screen is up" and knows nothing about the run;
// savefile's cat-id test answers "is a run loaded" and knows nothing about the
// screen. They are not redundant, and a module that consults both still has an
// answer when one of them has stopped working -- which is the difference
// between telling a player their host left and saying nothing at all.
enum class Where { Unknown, InRun, OutOfRun };

struct SceneList {
    uint32_t count = 0;
    char     name[kMaxScenes][40] = {};
    bool     dying[kMaxScenes] = {};
};

struct State {
    bool on        = false;
    bool is_client = false;
    bool broken    = false;   // the scene walk did not read as a scene list
    bool said_broken = false;
    uint32_t broken_polls = 0;   // consecutive failures; see report_broken
    bool printed   = false;   // the one-shot roster of loaded scenes

    const void** director_slot = nullptr;
    void  (*button_click)(void*, bool) = nullptr;

    uint32_t tick = 0;

    // --- host ---
    bool     was_in_run = false;   // has this peer been inside a run at all
    uint32_t out_polls  = 0;
    bool     announced  = false;
    uint32_t sent       = 0;

    // --- the chapter page, by its buttons ticking (see leave_on_button) ---
    uint32_t chapter_tick = 0;      // the frame one of the two buttons last updated
    bool     said_chapter = false;  // the "we are on it" line, once per process
    bool     saw_map      = false;  // the adventure map has been up (see kScene_Map)

    // --- client ---
    bool     pending      = false;  // told to leave, still in a run
    uint32_t presses      = 0;
    uint32_t cooldown     = 0;
    bool     said_gave_up = false;
    bool     said_no_click = false;
    bool     said_sidebar  = false;   // the pause sidebar was seen ticking
    bool     said_held     = false;   // the held press was explained (see
                                      // leave_on_button_update): the hold is silent by
                                      // construction, and "nothing happened" needs a line
    uint32_t left         = 0;      // departures actually completed
    uint32_t received     = 0;

    // THE PLAYER'S ANSWER to "the other side has left" (2026-09-22). `pending` says a
    // departure has been announced; this says what the person at THIS keyboard wants
    // to do about it, and Undecided is the state the run is held in -- see
    // leave_decide_* and mgmp_ui's co-op session section.
    enum class Choice { Undecided, ContinueAlone, Wait };
    Choice choice = Choice::Undecided;

    // CONTINUING ALONE, AND NOT PAIRED AGAIN UNTIL THE WAREHOUSE (2026-09-22).
    //
    // The user's rule: a peer that chose to continue alone may not pair again until both
    // players are back at a warehouse. Held here as a latch of its own rather than read
    // off `choice`, because the two answer different questions: `choice` is what the
    // player said about one departure, and this is a standing condition that outlives it
    // (a second departure, or a re-armed question, must not reopen pairing).
    //
    // It clears in the House (see leave_pump) and it is what leave_pairing_allowed
    // consults before a host or join request is accepted.
    bool solo = false;
    bool settled = false; // suppress old scene/turn readings until the next run

    // THE LAST POSITION THE PUMP READ, so nothing else has to walk the scene
    // list to find out where we are.
    //
    // leave_status runs from the panel's render, i.e. every frame, and it was
    // taking the full walk each time: a pointer chase and a std::string read
    // per loaded scene, sixty times a second, for a line of text. During a
    // scene teardown those reads land on freed Scene objects and fault -- caught,
    // but the faults were real and they were the ones filling the crash log on
    // 2026-08-28. Polling twice a second is already far finer than the event
    // being watched; reading it sixty times a second bought nothing.
    Where    where       = Where::Unknown;
    char     where_name[40] = {};
};

State g;

// Same de-latched shape as savefile's, and for the same reason: the role can be
// set by the panel's connect buttons long after the first call, so an answer
// computed once is an answer that stays wrong for the life of the process.
void ensure_state() {
    const Config& cfg = config();
    const bool host   = _stricmp(cfg.net_role, "host")   == 0;
    g.is_client       = _stricmp(cfg.net_role, "client") == 0;
    g.on              = (host || g.is_client);
}

// Walk the game's loaded-scene vector. Pure reads; see the kScene_* block in
// mgmp_addresses.h.
//
// GATED ON EVIDENCE, NOT ON PERFECTION, and the difference is the whole reason
// this reads the way it does now. The first version required EVERY entry to
// yield a non-empty printable name and returned false otherwise -- so a single
// scene with no name, or one name longer than the buffer, disabled the module
// outright and took the "the host left the run" announcement with it. That is
// the wrong shape for a heuristic: "at least one entry read as a scene name"
// already rules out an unrelated pair of pointers, and unreadable entries can
// simply be skipped, because the question being asked of this list is whether a
// PARTICULAR name is present.
//
// So an entry that does not read leaves an empty slot and the walk continues;
// the walk fails only if the vector itself is implausible or NOTHING in it read
// as a name.
bool read_scenes(SceneList& out) {
    out.count = 0;
    if (!g.director_slot) return false;

    const void* director_owner = nullptr;   // the MewDirector, itself a Component
    if (!mem_read(g.director_slot, &director_owner, sizeof(director_owner)) ||
        !director_owner)
        return false;

    const void* director = nullptr;
    if (!mem_read((const uint8_t*)director_owner + kDir_SceneDirector,
                  &director, sizeof(director)) || !director)
        return false;

    const uint8_t* begin = nullptr;
    const uint8_t* end   = nullptr;
    if (!mem_read((const uint8_t*)director + kDirector_ScenesBegin, &begin, sizeof(begin)))
        return false;
    if (!mem_read((const uint8_t*)director + kDirector_ScenesEnd,   &end,   sizeof(end)))
        return false;
    if (!begin || end < begin) return false;

    const size_t bytes = (size_t)(end - begin);
    if (bytes % sizeof(void*)) return false;
    const size_t n = bytes / sizeof(void*);
    if (n == 0 || n > kMaxScenes) return false;

    uint32_t named = 0;
    for (size_t i = 0; i < n; ++i) {
        char* dst = out.name[i];
        dst[0] = 0;
        out.dying[i] = false;

        const void* scene = nullptr;
        if (!mem_read(begin + i * sizeof(void*), &scene, sizeof(scene)) || !scene)
            continue;
        if (!mem_read_std_string((const uint8_t*)scene + kScene_Name, dst,
                                 sizeof(out.name[0]))) {
            dst[0] = 0;
            continue;
        }

        const size_t len = strlen(dst);
        bool printable = len > 0;
        for (size_t c = 0; c < len && printable; ++c)
            if (dst[c] < 0x20 || dst[c] > 0x7E) printable = false;
        if (!printable) { dst[0] = 0; continue; }

        uint8_t dying = 0;
        if (mem_read((const uint8_t*)scene + kScene_Destroying, &dying, 1))
            out.dying[i] = (dying != 0);
        ++named;
    }
    // Nothing read as a name: this is not a scene vector.
    if (!named) return false;
    out.count = (uint32_t)n;
    return true;
}

// Which of the three "not in a run" scenes is live, or nullptr.
const char* out_scene(const SceneList& s) {
    static const char* const kOut[] = { kScene_House, kScene_MainMenu, kScene_SaveSelect };
    for (uint32_t i = 0; i < s.count; ++i) {
        if (s.dying[i]) continue;
        for (const char* k : kOut)
            if (strcmp(s.name[i], k) == 0) return k;
    }
    return nullptr;
}

// One line naming everything the game has loaded, once. This is the cheapest
// possible confirmation that Director+0/+8 really is a vector of scenes and
// Scene+1208 really is its name -- three or four recognisable names in the log
// prove it, and a line of nonsense refutes it, without anyone reading the
// disassembly again.
void print_once(const SceneList& s) {
    if (g.printed) return;
    g.printed = true;
    char line[512];
    int  off = 0;
    for (uint32_t i = 0; i < s.count && off < (int)sizeof(line) - 48; ++i)
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "%s%s%s",
                           i ? ", " : "", s.name[i], s.dying[i] ? "(dying)" : "");
    log_line_lvl(LogLevel::Trace, "LEAVE", "scenes loaded: %s", line);
}

// Takes the reading. THE ONLY CALLERS ARE THE PUMP AND leave_on_message --
// everything else reads g.where, which this updates. See the State comment.
Where where_are_we(const char** out_name) {
    if (out_name) *out_name = nullptr;

    // THE POSITIVE HALF, AND IT IS NOT OPTIONAL. "In a run" used to mean only
    // that none of the three out-scenes was loaded -- an absence, which is a
    // reading nothing has to be true for.
    //
    // At startup nothing is true. The game boots into
    //   Shared, Base, Tutorial, Cutscene, PauseMenu, ToolTip, Transition, ...
    // with no MainMenu scene yet, so the absence test said IN A RUN, the host
    // recorded that it had been in one, and the instant MainMenu finished
    // loading it read OUT and announced a departure that never happened. The
    // client believed it and quit to the menu -- measured 2026-08-28, host seq
    // 35 then 37, client seq 34 then 35, all before a save had even been
    // picked. The asymmetry that stops a transition gap from manufacturing a
    // departure does not help here, because this gap is not transient: it lasts
    // the whole boot sequence.
    //
    // So a run has to be POSITIVELY established, and the cat-id list is the
    // reading that can do it -- it is populated by ContinueAdventure and empty
    // on every menu. Absence of an out-scene is now necessary and not
    // sufficient, and anything else is Unknown, which never announces.
    const bool run_loaded = savefile_adventure_is_loaded();

    SceneList s{};
    if (read_scenes(s)) {
        if (const char* out = out_scene(s)) {
            if (out_name) *out_name = out;
            return Where::OutOfRun;
        }
        return run_loaded ? Where::InRun : Where::Unknown;
    }

    // The scene list did not read. The cat-id test cannot say WHICH screen is
    // up, so the caller loses the scene name in its log line and nothing else --
    // but it can still say a run is loaded, and that is the half that gates the
    // announcement.
    return run_loaded ? Where::InRun : Where::Unknown;
}

// Take a reading and remember it. Returns the same thing where_are_we does.
Where refresh_where(const char** out_name) {
    const char* named = nullptr;
    g.where = where_are_we(&named);
    strncpy_s(g.where_name, named ? named : "", _TRUNCATE);
    if (out_name) *out_name = g.where_name[0] ? g.where_name : nullptr;
    return g.where;
}

void report_broken() {
    // A TRANSIENT EMPTY VECTOR IS NOT A BROKEN MODULE, and treating it as one cost
    // a round of this investigation.
    //
    // On the client the first poll lands before the game has loaded a single
    // scene, `read_scenes` returns false for an empty vector, and the log got a
    // red "this peer cannot tell whether the host is still in the run" -- while
    // THE VERY NEXT POLL read the list perfectly and printed it (`scenes loaded:
    // Shared, Base, Tutorial, ...`). Because said_broken latches, every later
    // reader of that log -- including the notes in the design notes -- believed this
    // peer's leave detection was dead. It never was; the module works, and the
    // only broken thing was the alarm.
    //
    // So it takes three consecutive failures, which is the same "confirm before
    // you announce" rule the OUT detection below already uses for the opposite
    // direction. The flag describes the last few reads, not the process.
    if (++g.broken_polls < kBrokenPolls) return;
    if (g.said_broken) return;
    g.said_broken = true;
    g.broken = true;
    log_line_lvl(LogLevel::Error, "LEAVE",
                 "!! the loaded-scene list did not read as one (Director+0/+8, "
                 "Scene+%u name, Scene+%u destroy flag) -- this peer cannot tell "
                 "whether the host is still in the run, so nobody will be told "
                 "when it leaves. Re-derive those offsets after a game update.",
                 (unsigned)kScene_Name, (unsigned)kScene_Destroying);
}

} // namespace

// ---------------------------------------------------------------------------

void leave_init() {
    ensure_state();
    g.was_in_run = false;
    g.out_polls  = 0;
    g.announced  = false;
    g.pending    = false;
    g.presses    = 0;
    g.cooldown   = 0;
    g.said_gave_up = false;
    g.said_held    = false;
    g.chapter_tick = 0;
    g.said_chapter = false;
    g.saw_map      = false;
    g.settled      = false;
    g.tick       = 0;
    g.where      = Where::Unknown;
    g.where_name[0] = 0;
    log_line_lvl(LogLevel::Trace, "LEAVE",
                 g.is_client
                     ? "armed -- if the host leaves the run, this peer is taken "
                       "back to the main menu"
                     : "armed -- will tell the client when this peer leaves the run");
}

void leave_shutdown() {
    if (g.on) {
        // A client that was told to leave and never managed it is the failure
        // worth seeing: it is still sitting in a dead run. Everything else here
        // is bookkeeping.
        const bool failed = g.pending;
        log_line_lvl(failed ? LogLevel::Error : LogLevel::Trace, "LEAVE",
                 "done: %u departure(s) announced, %u received, %u acted on%s",
                 g.sent, g.received, g.left,
                 failed ? " -- AND THIS PEER IS STILL IN A RUN THE HOST HAS LEFT; "
                          "open the pause menu and quit to the menu"
                        : "");
    }
    g.on = false;
    g.pending = false;
}

void leave_set_base(uintptr_t /*base*/) {
    ensure_state();
    g.director_slot = (const void**)addr_of_data(D_MewDirectorPtr);
    g.broken = false;
    g.said_broken = false;
    g.broken_polls = 0;

    // Shared with savefile's auto-Play, and it turns off the same way: without
    // it this module can still SAY the host left, which is most of the value --
    // the client just has to press the button itself.
    g.button_click = nullptr;
    const uintptr_t click = addr_of_call(C_ButtonClick);
    if (!click)
        log_line_lvl(LogLevel::Error, "LEAVE",
                     "!! %s did not resolve by signature -- this peer can be told "
                     "the host left the run but cannot press Quit To Menu for you",
                     kCalls[C_ButtonClick].name);
    else
        g.button_click = (void(*)(void*, bool))click;
}

// The cached reading -- see the State comment. Half a second stale at worst,
// which is finer than the thing it describes changes.
bool leave_in_run() { return g.where == Where::InRun; }

// Called from the Button::update detour for every button in the game, so the
// cheap test is first: two string compares against a name that is only read when
// the caller has already decided this frame matters. See the note on
// kBtnName_ChapterLower for why the page is identified by its buttons rather
// than by its scene name.
void leave_on_button(void* button) {
    if (!g.on || !button) return;

    char name[64];
    if (!mem_read_std_string((const uint8_t*)button + kBtn_Name, name, sizeof(name)))
        return;
    if (strcmp(name, kBtnName_ChapterLower) != 0 &&
        strcmp(name, kBtnName_ChapterRaise) != 0)
        return;

    const bool first = (g.chapter_tick == 0) || !leave_at_chapter_page();
    g.chapter_tick = g.tick;

    // The scene name goes in the line on purpose. It is the one thing this
    // detection deliberately does NOT depend on, so printing it is a free
    // cross-check: if the two ever disagree, the log says so at the moment the
    // barrier would have used the wrong one.
    if (first && !g.said_chapter) {
        g.said_chapter = true;
        log_line("LEAVE", "the chapter page is up (its two difficulty buttons are"
                          " ticking) -- scene is '%s', and this peer is %s",
                 g.where_name[0] ? g.where_name : "unreadable",
                 g.where == Where::InRun ? "in a run"
                                         : (g.where == Where::OutOfRun ? "out of one"
                                                                       : "nowhere known"));
    }
    // Keep calling this while the chapter buttons tick: a save can finish
    // constructing its party one frame after the first button update.
    setup_on_chapter_page();
    g.settled = false; // a new departure is now positively established
}

// The House SPECIFICALLY, from the same cached reading: "out of a run" plus the
// name that says which out-scene it is.
//
// mgmp_savefile needs exactly this and nothing broader: a client that has just
// loaded the host's run lands in the House (MewDirector::init always ends there)
// and the run only starts when somebody walks out of the door. Asking for the
// name reuses this module's own poll instead of adding a second scene walk that
// could drift on its own -- the arrangement the top of this file argues for.
bool leave_in_house() {
    return g.where == Where::OutOfRun && strcmp(g.where_name, kScene_House) == 0;
}

// Fresh within half a second, which is the same freshness the poll above has and
// is why the test is a heartbeat rather than a flag: the buttons stop updating
// the moment their screen goes away, so a stale tick means the page is gone.
bool leave_at_chapter_page() {
    return g.chapter_tick != 0 && (g.tick - g.chapter_tick) <= kChapterFrames;
}

// Latched, so it survives the screen going away -- which is the whole point: the
// question is "has this peer got past the setup in this run", and it has to stay
// true for the rest of the run.
//
// TWO WAYS TO KNOW, and both are needed. The chapter page is where a NEW run
// passes; the map is where a RESUMED one arrives, without ever seeing the House or
// the chapter screen. Either one means the run has begun and its state is worth
// sending.
bool leave_setup_done() { return g.said_chapter || g.saw_map; }

void leave_pump() {
    ensure_state();

    // THE COOLDOWN IS SERVICED BEFORE EVERY EARLY RETURN, and that ordering is
    // the whole bug this function shipped with.
    //
    // It is counted here rather than in the button callback because that
    // callback runs once per BUTTON per frame: decremented there, a screen with
    // ten live buttons drains a 120-frame cooldown in twelve, so all five
    // allowed presses land inside a fifth of a second -- the same press five
    // times before anything could respond to the first.
    //
    // But it sat BELOW `if (!g.on || !net_active()) return;`, so on any frame
    // that guard fired the cooldown was never counted down. One press parked it
    // at 120 forever and the button callback returned at `if (g.cooldown)` from
    // then on. THE FEATURE WORKED EXACTLY ONCE PER PROCESS and then went silent
    // -- reported as "worked in the battle, then did not work on the adventure
    // or in a battle either". A counter that gates the only action a module
    // takes must be counted unconditionally.
    if (g.cooldown) --g.cooldown;

    // Nothing armed and no role: there is nothing to watch for. `g.pending` is
    // in the test because leave_request_local arms a peer that may have no
    // session at all, and the "did we get out" check below has to run for it.
    if (!g.on && !g.pending) return;

    if (++g.tick % kPollFrames) return;

    // Printed once when it works, and the failure is reported once too --
    // without this the module's whole input was invisible, which is exactly
    // what made the first report of "quit to menu did nothing" unanswerable.
    SceneList s{};
    if (read_scenes(s)) {
        g.broken_polls = 0;
        print_once(s);

        // THE OTHER HALF OF "THE RUN HAS STARTED": the map being up. A resumed
        // run loads straight onto it -- no House, no chapter screen -- so a
        // chapter-only test left such a run looking like it had never begun, and
        // the client waited on the menu for a save that was never sent. Measured
        // 2026-09-22.
        for (uint32_t i = 0; !g.settled && i < s.count && !g.saw_map; ++i)
            if (!s.dying[i] && strcmp(s.name[i], kScene_Map) == 0) g.saw_map = true;
    } else {
        report_broken();
    }

    const char* out = nullptr;
    const Where here = refresh_where(&out);
    const char* where_name = out ? out : "somewhere this peer cannot name";

    // --- the THIRD answer: the adventure was abandoned by hand -------------
    //
    // The user's rule (2026-09-22): "if, while held, the player chooses to abandon the
    // adventure and go back to the warehouse -- i.e. ends this adventure by hand -- that
    // also counts as giving up the co-op session."
    //
    // The pause menu's fifth entry does exactly that, and its DESTINATION is the House --
    // the room this module can already read (leave_in_house, the same test savefile uses
    // for "walk out of the door"). So the release needs no button name and no new
    // detection: while a question is pending, ARRIVING IN THE HOUSE means the run was
    // abandoned by hand.
    //
    // Released rather than blocked, deliberately. The hold exists so the peer ANSWERS
    // instead of drifting; a player who has already given the adventure up has answered
    // by the only means that matters, and there is no run left to hold.
    //
    // Placed before the session test below, because this is also the one case where the
    // session is closed FROM HERE: the user's rule says abandoning the adventure gives up
    // the co-op too, and leaving the socket up would let the peer's next save drag this
    // one back into a run it has just walked away from.
    // A SOLO PEER IS PAIRABLE AGAIN AT THE WAREHOUSE (2026-09-22).
    //
    // The user's rule -- "the two of you cannot pair again until both are back at a
    // warehouse" -- has to have a release point, and the House is it (the same reading
    // that identifies the abandon below). Checked before the question, because it is
    // about the standing latch rather than about a decision.
    if (g.solo && leave_in_house()) {
        g.solo = false;
        log_line("LEAVE", "in the House (the warehouse) -- pairing is allowed again, as the"
                          " co-op rule says");
    }

    if (g.pending && leave_in_house()) {
        const bool was_undecided = (g.choice == State::Choice::Undecided);
        g.pending = false;
        g.choice  = State::Choice::Undecided;   // no run left, so nothing to remember
        g.said_sidebar = false;
        g.said_held    = false;
        ++g.left;
        log_line_lvl(LogLevel::Warn, "LEAVE",
                     "the adventure was abandoned by hand -- this peer is in the House"
                     " (the warehouse), which is where the pause menu's \"abandon the"
                     " adventure\" puts it. Read as giving up the co-op session: the %s"
                     " question is released and the session is closed.",
                     was_undecided ? "unanswered" : "answered");
        if (g.on) session_request_disconnect();
        return;
    }

    // --- we were leaving, and we got out ----------------------------------
    //
    // Before the role split, because leave_request_local can arm a host and a
    // departure that completes has to be reported whoever asked for it.
    if (g.pending && here == Where::OutOfRun) {
        g.pending = false;
        ++g.left;
        log_line("LEAVE", "out of the run and on '%s' -- if the host picks a save "
                          "again this peer will follow it back in on its own",
                 where_name);
    }
    // A FRESH RUN IS A FRESH CHANCE. The press budget is per departure, not per
    // process: without this, a peer that spent its five presses once could
    // never be taken out of a later run, and the only symptom would be the
    // gave-up line from the previous one.
    if (here == Where::InRun && (g.presses || g.said_gave_up)) {
        g.presses      = 0;
        g.said_gave_up = false;
    }

    // Everything below is the ANNOUNCING half, and only that half needs a live
    // session -- the arming and the click above do not.
    if (!g.on || !net_active()) return;
    if (g.settled) return;
    if (g.is_client) return;

    // --- the host: announce leaving, exactly once per run ------------------
    //
    // ONLY A POSITIVE "IN A RUN" ARMS THE ANNOUNCEMENT. Unknown suppresses --
    // it neither arms nor announces -- which is the same asymmetry as before
    // read the other way round: a reading we could not take must never become
    // half of "was in a run, now is not". That pair is the whole claim.
    if (here == Where::InRun) {
        // ARMING TAKES MORE THAN "A RUN IS LOADED", AND THAT COST A SESSION.
        //
        // Measured 2026-09-22, host log 000105: the slot click loaded a save that
        // already had an adventure in it, so the cat-id list was populated and no
        // out-scene was up yet -- `here` said InRun at seq 51 -- and then the
        // House appeared and the run was torn down, which read as InRun -> OutOfRun
        // and announced a DEPARTURE (seq 54). The client obeyed: it pressed Quit
        // To Menu for itself and landed on MainMenu, and when the host entered its
        // first node the client's own log said `0 followed`. A player who never
        // left anything was taken out of the run, by a pair of readings that
        // individually were both correct.
        //
        // The missing distinction is LOADED versus STARTED. A run that was merely
        // loaded is still on its way to the House, where the player chooses the
        // party and the chapter; the run has not begun, so nothing can be left.
        // The signal for "it began" is the chapter page -- the first screen that
        // exists only after the setup is complete -- and it is the same latch the
        // first save publish waits on (leave_setup_done). One fact, one flag.
        const bool started = leave_setup_done() || lockstep_in_battle();
        if (started && !g.was_in_run)
            log_line("LEAVE", "the host is in a run -- the client will be told if "
                              "it leaves");
        g.was_in_run = g.was_in_run || started;
        g.out_polls  = 0;
        g.announced  = false;      // re-arm: a NEW run may be left later
        return;
    }
    if (here == Where::Unknown) {
        g.out_polls = 0;           // a boot screen or a transition, not a departure
        return;
    }
    // Never been in a run this session -- sitting on the menu at startup is not
    // a departure, and announcing it would take a client out of a run it had
    // legitimately been caught up into.
    if (!g.was_in_run || g.announced) return;
    if (++g.out_polls < kConfirmPolls) return;

    g.announced  = true;
    g.was_in_run = false;
    ++g.sent;

    HostLeftMsg m{};
    strncpy_s(m.scene, out ? out : "", _TRUNCATE);
    if (net_send_hostleft(m))
        log_line("LEAVE", "-> the host has left the run and is on '%s' -- telling "
                          "the client, which is otherwise unable to tell this "
                          "apart from a long turn", where_name);
    else
        log_line_lvl(LogLevel::Error, "LEAVE",
                 "!! the host left the run and the message could not be sent -- "
                 "the client is still in a run nobody is playing");
}

void leave_on_message(const HostLeftMsg& m) {
    ensure_state();
    if (!g.on) return;
    if (g.settled) return; // a late departure cannot reopen a completed run
    ++g.received;

    if (!g.is_client) {
        // Symmetric message, asymmetric authority -- the same failure mode
        // savefile and follow both report when both peers think they own the
        // run.
        log_line("LEAVE", "!! received a 'host left the run' while hosting -- both "
                          "peers believe they own the run");
        return;
    }

    const Where here = refresh_where(nullptr);

    // ONLY "definitely out" declines. Unknown ARMS, and that is the correction:
    // the first version bailed with a warning whenever it could not read its own
    // position, which is a drop site that does nothing -- the rule this project
    // keeps relearning. Arming on Unknown costs nothing, because the only thing
    // it can do is click a Quit To Menu button, and that button does not exist
    // unless the player is in a run or the house and has opened the pause menu.
    if (here == Where::OutOfRun) {
        // The common case when the host is merely re-picking a slot: it went out
        // to the menu, we were already there. Not a problem, and not worth
        // interrupting anyone for.
        log_line_lvl(LogLevel::Trace, "LEAVE",
                 "the host has left the run (it is on '%s'); this peer is not in a "
                 "run either, so there is nothing to do", m.scene);
        return;
    }

    g.pending      = true;
    g.presses      = 0;
    g.cooldown     = 0;
    g.said_gave_up = false;

    g.said_sidebar = false;
    g.said_held    = false;                      // the hold explains itself once per question
    g.choice       = State::Choice::Undecided;   // the question this peer must answer

    // THE LINE A PLAYER MUST NOT MISS, and the reason it names a key. There is
    // no persistent PauseMenu to reach into -- see the header -- so the one
    // thing this peer cannot do for itself is open the menu.
    log_line_lvl(LogLevel::Warn, "LEAVE",
             "the host has LEFT THE RUN (it is on '%s')%s. Press Escape: the mod "
             "will press Quit To Menu for you, and if the host starts a run again "
             "you will be taken back in automatically.",
             m.scene,
             here == Where::InRun
                 ? " and this peer is still in it"
                 : " and this peer cannot tell whether it is still in one, so it "
                   "is arming anyway");
}

void leave_status(char* out, size_t out_size) {
    if (!out || !out_size) return;
    out[0] = 0;

    // The CACHED reading. This runs once per rendered frame and must not walk
    // the game's scene list to do it -- see the State comment.
    const char* place = "?";
    switch (g.where) {
        case Where::InRun:    place = "in a run";                             break;
        case Where::OutOfRun: place = g.where_name[0] ? g.where_name : "out"; break;
        default:              place = "cannot tell";                          break;
    }

    if (!g.pending) {
        _snprintf_s(out, out_size, _TRUNCATE, "idle -- %s%s", place,
                    g.button_click ? "" : "  [Button::Click UNRESOLVED]");
        return;
    }
    _snprintf_s(out, out_size, _TRUNCATE,
                "%s -- %s, %u press(es) left%s%s",
                g.choice == State::Choice::Undecided   ? "WAITING FOR YOUR ANSWER"
              : g.choice == State::Choice::Wait        ? "waiting for the peer"
                                                       : "continuing alone",
                place,
                g.presses < kMaxPresses ? kMaxPresses - g.presses : 0,
                g.cooldown ? "  (waiting out the last press)" : "",
                g.said_sidebar ? "  [pause menu seen]"
                               : "  [pause menu NOT seen yet]");
}

void leave_request_local() {
    ensure_state();
    g.pending      = true;
    g.presses      = 0;
    g.cooldown     = 0;
    g.said_gave_up = false;
    g.said_sidebar = false;
    g.said_held    = false;
    g.choice       = State::Choice::Undecided;
    // g.on is NOT required here and that is the point: the click path does not
    // depend on a session, so neither should the test of it. What it does need
    // is Button::Click, which is resolved in leave_set_base at load time.
    g.on = true;
    log_line("LEAVE", "armed by hand from the panel%s -- open the pause menu and "
                      "Quit To Menu will be pressed for you",
             g.button_click ? "" : ", BUT Button::Click did not resolve, so "
                                   "nothing can be pressed");
}

bool leave_decision_pending() {
    return g.pending && g.choice == State::Choice::Undecided;
}

// THE PARTY, REWRITTEN TO THIS PEER'S OWN CATS (2026-09-22).
//
// "Continue alone" means playing this run with MY cats, and the layer that decides who
// fights is the RUN's cat id vector -- MewDirector+1468 (count) / +1472 (data), the same
// pair mgmp_roster and mgmp_catsync walk. So that vector is what gets rewritten.
//
// WHO IS MINE comes from lockstep_cat_is_mine, which mgmp_catsync already trusts for this
// exact question on the map (lockstep copies the control split out of the battle and
// keeps it between nodes). Its contract decides the safety rule here, and the rule is not
// symmetric: FALSE MEANS "NOBODY HAS TOLD ME YET", so a cat whose owner cannot be PROVEN
// is KEPT. Leaving one of the other player's cats in costs a smaller party than it should
// be; deleting one of MY OWN destroys the run. The conservative direction is the only
// defensible one.
//
// WRITTEN AND READ BACK, both fields, and the ids are printed on both sides of the change:
// ids are the only form of this that another log can be checked against.
//
// AND THE WIDTH IS EIGHT BYTES. mgmp_roster's shrink code reads its display copy as four
// -- harmless there, because it only ever wrote the count -- but reading four here would
// carve every other id into halves, so this follows mgmp_catsync's reading instead.
//
// OWNERSHIP NOTE, stated rather than hidden: the run's party is mgmp_roster's territory,
// and this write lives in mgmp_leave anyway. The reason is a calling convention -- this
// file already includes the memory, address and lockstep layers the job needs, and putting
// it in roster.cpp would have dragged in a new include to answer an ownership question
// that is not roster's. If a third caller ever needs this, move it and take the include.
bool keep_own_cats_only() {
    // The module's OWN resolved pointer, not a fresh base+rva arithmetic: this file has no
    // windows.h and does not need one, and leave_set_base already resolved exactly this
    // slot (addr_of_data(D_MewDirectorPtr)). One resolution, one source of truth.
    const void** slot = g.director_slot;
    const void* md = nullptr;
    if (!slot || !mem_read(slot, &md, sizeof(md)) || !md) {
        log_line_lvl(LogLevel::Error, "LEAVE",
                     "!! continue-alone: the MewDirector pointer is not readable -- the"
                     " other player's cats are still in the party");
        return false;
    }

    uint32_t  count = 0;
    uintptr_t data  = 0;
    if (!mem_read((const uint8_t*)md + kDir_CatIdCount, &count, 4) ||
        !mem_read((const uint8_t*)md + kDir_CatIdData, &data, sizeof(data)) || !data ||
        !count || count > 64) {
        log_line_lvl(LogLevel::Error, "LEAVE",
                     "!! continue-alone: MewDirector+%u/+%u did not read as the run's cat id"
                     " vector (count %u, data 0x%llX) -- nothing rewritten",
                     (unsigned)kDir_CatIdCount, (unsigned)kDir_CatIdData,
                     count, (unsigned long long)data);
        return false;
    }

    uint64_t ids[64] = {};
    if (!mem_read((const void*)data, ids, count * (uint32_t)sizeof(uint64_t))) {
        log_line_lvl(LogLevel::Error, "LEAVE",
                     "!! continue-alone: the party of %u cat(s) is not readable -- nothing"
                     " rewritten", count);
        return false;
    }

    uint64_t out[64] = {};
    uint32_t kept = 0, dropped = 0, unknown = 0;
    for (uint32_t i = 0; i < count; ++i) {
        bool mine = false;
        const bool known = ids[i] && lockstep_cat_is_mine(ids[i], mine);
        if (known && !mine) { ++dropped; continue; }
        if (!known) ++unknown;
        out[kept++] = ids[i];
    }

    char before[320] = {}, after[320] = {};
    for (uint32_t i = 0, o = 0; i < count && o < (int)sizeof(before) - 20; ++i)
        o += _snprintf_s(before + o, sizeof(before) - o, _TRUNCATE, "%s%llx",
                         i ? " " : "", (unsigned long long)ids[i]);
    for (uint32_t i = 0, o = 0; i < kept && o < (int)sizeof(after) - 20; ++i)
        o += _snprintf_s(after + o, sizeof(after) - o, _TRUNCATE, "%s%llx",
                         i ? " " : "", (unsigned long long)out[i]);

    if (!dropped) {
        log_line_lvl(LogLevel::Warn, "LEAVE",
                     "continue-alone: the party of %u cat(s) [%s] is unchanged -- %u cat(s)"
                     " could not be attributed to a player and were KEPT (a split that has"
                     " not happened yet, or non-roster cats), and none was PROVEN to be the"
                     " other player's", count, before, unknown);
        return true;
    }
    if (!kept) {
        log_line_lvl(LogLevel::Error, "LEAVE",
                     "!! continue-alone: every one of the %u cat(s) [%s] was attributed to"
                     " the other player -- REFUSING, because an empty party is not a state"
                     " the game builds and this is far more likely to be a wrong reading"
                     " than a run with no cats of mine", count, before);
        return false;
    }

    if (!mem_write((uint8_t*)data, out, kept * (uint32_t)sizeof(uint64_t))) {
        log_line_lvl(LogLevel::Error, "LEAVE",
                     "!! continue-alone: the party array is not writable -- the run is"
                     " untouched");
        return false;
    }
    uint64_t check[64] = {};
    mem_read((const void*)data, check, kept * (uint32_t)sizeof(uint64_t));
    const bool arr_ok = memcmp(check, out, kept * (uint32_t)sizeof(uint64_t)) == 0;

    uint32_t cnt_back = 0;
    const bool cnt_ok = mem_write((uint8_t*)md + kDir_CatIdCount, &kept, 4) &&
                        mem_read((const uint8_t*)md + kDir_CatIdCount, &cnt_back, 4) &&
                        cnt_back == kept;

    log_line_lvl(LogLevel::Warn, "LEAVE",
                 "!! EDITED THE RUN: continuing alone, the party was %u cat(s) [%s] and is"
                 " now %u [%s] -- %u dropped as PROVEN to be the other player's, %u kept as"
                 " UNATTRIBUTED. Array %s, count %u -> %u (%s) at MewDirector+%u. THIS"
                 " AFFECTS THE NEXT BATTLE the game builds, not any fight in progress.",
                 count, before, kept, after, dropped, unknown,
                 arr_ok ? "written and read back" : "READ-BACK DISAGREES",
                 count, kept, cnt_ok ? "written and read back" : "READ-BACK DISAGREES",
                 (unsigned)kDir_CatIdCount);
    return arr_ok && cnt_ok;
}

bool leave_pairing_allowed(const char* what) {
    ensure_state();
    if (!g.solo) return true;
    if (leave_in_house()) return true;      // the warehouse releases it; see leave_pump
    log_line_lvl(LogLevel::Warn, "LEAVE",
                 "refused to start %s: this peer is CONTINUING ALONE, and the rule is that"
                 " the two of you cannot pair again until both are back at a warehouse --"
                 " this peer is not there yet", what ? what : "a session");
    return false;
}

bool leave_solo_active() {
    ensure_state();
    return g.solo;
}

void leave_decide_continue_alone() {
    ensure_state();
    if (!checkpoint_on_alone()) return;
    g.choice = State::Choice::ContinueAlone;

    const bool first = !g.solo;
    g.solo = true;

    log_line_lvl(LogLevel::Warn, "LEAVE",
                 "the player chose to CONTINUE ALONE -- this peer keeps this run with its"
                 " own cats: following is dropped, the other player's cats are removed from"
                 " the party, and the session is closed. Pairing stays refused until both"
                 " players are back at a warehouse.");

    if (!first) return;                     // the work below is done once, not per click

    // 1. STOP FOLLOWING. A client's node clicks are swallowed by mgmp_follow -- that is
    //    the mechanism that made "the host drives the run" true -- and this peer is now
    //    its own driver. Shutting the module down is cleaner than teaching every one of
    //    its conditions a solo case: it exists to follow a host, and there is none.
    follow_shutdown();

    // 2. THE PARTY. Local, so it is safe before the session goes.
    keep_own_cats_only();

    // 3. THE SESSION, THROUGH THE SESSION'S OWN TEARDOWN -- and the first version of this
    //    called net_shutdown() directly, which was wrong in a way that took a session to
    //    show up.
    //
    //    net_shutdown() closes the sockets and leaves every co-op MODULE armed in a process
    //    that is now single-player. Measured 2026-09-22, right after "continue alone":
    //    clicking a save slot did nothing at all, and the log said why --
    //
    //        SAVEFILE suppressed local save pick: slot 1 (steamcampaign02.sav)
    //        -- the host's save is used (not received yet)
    //
    //    i.e. mgmp_savefile was still behaving as a client and swallowed the player's own
    //    click. The player was stuck on the menu with a run they could not load.
    //
    //    session_request_disconnect is the route that exists for exactly this: it records
    //    the request and applies it at the top of the next session_update -- the one point
    //    in the frame known to be between things -- and it runs the module shutdowns that
    //    the direct call skipped. One frame later is not a cost; a module that keeps
    //    acting for a session that is gone is.
    session_request_disconnect();
}

void leave_decide_wait() {
    ensure_state();
    g.choice = State::Choice::Wait;
    log_line("LEAVE", "the player chose to WAIT for the peer -- this is the old"
                      " behaviour: press Escape and Quit To Menu will be pressed for you.");
}

void leave_run_reset() {
    ensure_state();

    g.settled = false;
    g.was_in_run = false;
    g.out_polls = 0;
    g.announced = false;
    g.saw_map = false;
    g.said_chapter = false;
    g.chapter_tick = 0;

    // Silent when there is nothing to clear, because this is called on every fresh pick
    // and the common case is a peer with no question outstanding.
    g.presses = g.cooldown = 0;
    g.said_gave_up = false;
    if (!g.pending && g.choice == State::Choice::Undecided && !g.said_sidebar
        && !g.said_held)
        return;

    log_line("LEAVE", "a NEW RUN arrived -- clearing the %s decision this peer was"
                      " holding, because the run it was about is gone",
             g.choice == State::Choice::Wait        ? "waiting-for-the-peer"
           : g.choice == State::Choice::ContinueAlone ? "continue-alone"
                                                      : "undecided");

    g.pending      = false;
    g.choice       = State::Choice::Undecided;
    g.said_sidebar = false;
    g.said_held    = false;
}

void leave_on_settlement() {
    leave_run_reset();
    g.settled = true;
    g.solo = false;
    g.presses = g.cooldown = 0;
    g.said_gave_up = false;
    log_line("LEAVE", "native settlement completed; cleared departure state, keeping the co-op session");
}

void leave_on_button_update(void* button) {
    // The cheap tests first, in this order deliberately: this runs for every
    // button in the game on every frame, and g.pending is false for the whole
    // of a normal session.
    // NOT role-gated. g.pending is only ever set by a client receiving HOSTLEFT
    // or by leave_request_local, so the role has already been decided by
    // whoever set it -- and gating here would make the panel's test button do
    // nothing on the machine a developer is most likely sitting at.
    if (!g.pending || !button) return;

    // THE NAME IS READ BEFORE THE HOLD, and that reorder is the fix for a diagnostic
    // that lied (2026-09-22). The sidebar line below used to sit AFTER the hold, so a
    // peer genuinely staring at the pause menu kept reading `[pause menu NOT seen yet]`
    // on the panel: the observation had been gated together with the action, and the
    // panel said the hook had seen nothing when in fact it had. An observation must
    // never sit behind the gate it exists to explain.
    //
    // Cost, stated rather than hidden: while a question is pending this now reads a name
    // per button per frame, where it used to reach that read only on the WAIT branch.
    // The hold is a rare, human-paced state with a pause menu open, so it is paid
    // knowingly -- and it buys a panel line that cannot be wrong.
    char name[64];
    if (!mem_read_std_string((const uint8_t*)button + kBtn_Name, name, sizeof(name)))
        return;                              // not a named button, or not one

    // SAY WHETHER THE SIDEBAR WAS EVER SEEN, once. This is the line that makes
    // the next "I pressed Escape and nothing happened" answerable in one log
    // instead of a session: either this appears -- so the hook does see the
    // pause menu and the quit button simply is not in it under that name -- or
    // it does not, and the problem is upstream of this function entirely.
    if (!g.said_sidebar && strncmp(name, "Button_PauseMenu_", 17) == 0) {
        g.said_sidebar = true;
        log_line("LEAVE", "the pause menu is open (saw '%s') -- waiting for '%s'",
                 name, kBtnName_PauseQuitToMenu);
    }

    // A RUN HELD ON A QUESTION IS NOT A RUN TO LEAVE (2026-09-22). Pressing Quit To
    // Menu is now the WAIT branch's action rather than the default: an undecided peer
    // presses NOTHING, which is what makes "the mod blocks you until you answer" a
    // true statement instead of a modal over a peer that has already been yanked out.
    //
    // AND THE HOLD NOW SAYS SO, once. Silence was the design and it was also the whole
    // problem: "the button did nothing" and "the mod is holding it" look identical from
    // the keyboard, and telling them apart has already cost this project a session. The
    // line names the button and the two answers, so the log and the panel agree.
    if (g.choice != State::Choice::Wait) {
        if (!g.said_held && strcmp(name, kBtnName_PauseQuitToMenu) == 0) {
            g.said_held = true;
            log_line_lvl(LogLevel::Trace, "LEAVE",
                         "the pause menu is open and '%s' is up -- NOT pressed: this peer"
                         " is waiting for your answer (\"Continue alone\" or \"Wait for"
                         " the peer\"). Everything else on this screen works.",
                         name);
        }
        return;
    }

    if (!g.button_click) {
        if (!g.said_no_click) {
            g.said_no_click = true;
            log_line_lvl(LogLevel::Error, "LEAVE",
                         "the pause menu is open but Button::Click did not resolve "
                         "-- press Quit To Menu yourself");
        }
        return;
    }

    if (g.presses >= kMaxPresses) {
        if (!g.said_gave_up) {
            g.said_gave_up = true;
            log_line_lvl(LogLevel::Error, "LEAVE",
                         "pressed Quit To Menu %u times and this peer is still in "
                         "the run -- press it yourself", g.presses);
        }
        return;
    }

    if (g.cooldown) return;                  // counted down in leave_pump

    if (strcmp(name, kBtnName_PauseQuitToMenu) != 0) return;

    ++g.presses;
    g.cooldown = kRetryFrames;
    // SAY WHETHER A BATTLE IS BEING TORN DOWN. This is a marker, not a gate:
    // quitting mid-battle is something the game supports and a player does, so
    // refusing it here would be inventing a rule. But it is also the state in
    // which this mod is holding the most pointers into what is about to be
    // destroyed, so if a press is ever followed by a fault, the first question
    // is whether it was this press or a quiet one from the map -- and that
    // question has to be answerable from the log rather than from memory.
    log_line("LEAVE", "the host left the run -- pressing '%s' for you%s%s", name,
             g.presses > 1 ? " (again; the first press did not take)" : "",
             lockstep_in_battle() ? "  [a battle is live -- this tears it down]" : "");

    // force = 0, exactly as Button::update calls it. Forcing would bypass a
    // guard the game set for a reason and would hide a refusal worth hearing
    // about -- the same argument as the Play button.
    g.button_click(button, false);
}

MenuScreen leave_menu_screen() {
    static MenuScreen cached = MenuScreen::Unknown;
    static uint32_t   tick   = 0;
    if ((tick++ % 6) != 0) return cached;

    SceneList s{};
    if (!read_scenes(s)) { cached = MenuScreen::Unknown; return cached; }

    bool menu = false, select = false, house = false, other = false;
    for (uint32_t i = 0; i < s.count; ++i) {
        if (s.dying[i] || !s.name[i][0]) continue;
        if      (strcmp(s.name[i], kScene_MainMenu)   == 0) menu   = true;
        else if (strcmp(s.name[i], kScene_SaveSelect) == 0) select = true;
        else if (strcmp(s.name[i], kScene_House)      == 0) house  = true;
        else                                                other  = true;
    }
    (void)other;
    if (select)      cached = MenuScreen::SaveSelect;
    else if (house)  cached = MenuScreen::House;
    else if (menu)   cached = MenuScreen::MainMenu;
    else             cached = MenuScreen::Other;
    return cached;
}

bool leave_scene_live(const char* name) {
    if (!name || !name[0]) return false;
    SceneList s{};
    if (!read_scenes(s)) return false;
    for (uint32_t i = 0; i < s.count; ++i)
        if (!s.dying[i] && strcmp(s.name[i], name) == 0) return true;
    return false;
}

bool leave_scene_summary(char* out, size_t cap) {
    if (!cap) return false;
    out[0] = 0;
    SceneList s{};
    if (!read_scenes(s)) return false;
    size_t n = 0;
    for (uint32_t i = 0; i < s.count; ++i) {
        if (s.dying[i] || !s.name[i][0]) continue;
        n += _snprintf_s(out + n, cap - n, _TRUNCATE, "%s%s", n ? "," : "", s.name[i]);
    }
    return true;
}

} // namespace mgmp
