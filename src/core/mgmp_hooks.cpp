// mgmp_hooks.cpp -- phase 1: observe, never interfere.
//
// Every hook logs and tail-calls the original. Nothing here changes game state,
// consumes RNG, or takes a decision. The point is a trace we can diff between
// two instances launched on the same seed (phase 2), and a first empirical look
// at the TurnAction blob that the lockstep protocol has to carry.
//
// Argument roles below follow the MSVC x64 ABI applied to the mangled
// signatures recovered from the binary. TurnAction is passed by value and is
// larger than 8 bytes, so it arrives as a hidden pointer:
//
//   Ability::trigger(TurnAction)          rcx=this   rdx=&TurnAction
//   Character::DoAction(TurnAction, bool) rcx=this   rdx=&TurnAction   r8b=bool
//   Character::BeginTurn(int)             rcx=this   edx=int
//   Brain::GetChoice() -> TurnAction      rcx=this   rdx=&sret, returned in rax
//
// GetChoice's roles were open until the first battle trace settled them: rcx
// resolves through RTTI to a real glaiel::PlayerBrain, rdx is a stack address,
// and the returned pointer equals rdx. So `this` is in rcx and the hidden
// return buffer is in rdx -- the opposite of the "sret always lands in rcx"
// rule of thumb, which only holds for free functions.
//
// GetChoice is a *poll*, not a decision point. PlayerBrain::GetChoice is called
// over and over while the game waits for input, returning type=1 ("no decision
// yet") every time; 1695 of 1711 observed calls were that. Logging all of them
// buries the battle. So by default only decided results are logged, and
// Character::DoAction -- which fired exactly 12 times for 12 decisions -- is the
// real command boundary for lockstep.
#include "mgmp_ability.h"
#include "mgmp_addresses.h"
#include "mgmp_catsync.h"
#include "mgmp_equipment_guard.h"
#include "mgmp_combatlock.h"
#include "mgmp_cursor.h"
#include "mgmp_choice.h"
#include "mgmp_overlay.h"
#include "mgmp_invsync.h"
#include "mgmp_aim.h"
#include "mgmp_nodehash.h"
#include "mgmp_runhist.h"
#include "mgmp_config.h"
#include "mgmp_net.h"
#include "mgmp_tuning.h"
#include "mgmp_timedelay.h"
#include "mgmp_hooks.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_record.h"
#include "mgmp_replay.h"
#include "mgmp_resolve.h"
#include "mgmp_session.h"
#include "mgmp_lockstep.h"
#include "mgmp_spawntest.h"
#include "mgmp_follow.h"
#include "mgmp_savefile.h"
#include "mgmp_checkpoint.h"
#include "mgmp_setup.h"
#include "mgmp_unlocks.h"
#include "mgmp_leave.h"
#include "mgmp_page.h"
#include "mgmp_abandon.h"
#include "mgmp_room.h"
#include "mgmp_balance.h"
#include "mgmp_catview.h"
#include "mgmp_checkpoint.h"
#include "mgmp_listprobe.h"    // RE instrument; tune::kRosterProbe
#include "mgmp_menu.h"
#include "mgmp_roster.h"       // the run editor; config debug.roster_shrink
#include "mgmp_rng.h"
#include "mgmp_rtti.h"
#include "mgmp_turnaction.h"

#include <windows.h>
#include "MinHook.h"

#include <cstdio>
#include <cstring>
#include <intrin.h>   // _ReturnAddress

namespace mgmp {
namespace {

uintptr_t g_base = 0;

// Which detours are actually live. Written only by hooks_install, read by
// hooks_is_live -- see the header for why "the config asked for it" is a
// different and weaker claim.
bool g_live[T_COUNT] = {};

typedef void  (__fastcall* fn_this)(void* self);
typedef bool  (__fastcall* fn_unlock_chk)(void* save, void* name);
typedef int64_t (__fastcall* fn_prop_int)(void* props, void* key, int64_t fallback);
typedef void  (__fastcall* fn_this_i32)(void* self, int arg);
typedef void  (__fastcall* fn_ptr_i32)(void* a, int b);
typedef void  (__fastcall* fn_this_ptr)(void* self, void* ta);
typedef void  (__fastcall* fn_this_ptr_b)(void* self, void* ta, unsigned char flag);
typedef void* (__fastcall* fn_two_ptr)(void* a, void* b);
typedef void  (__fastcall* fn_ptr_i32_b)(void* a, int b, unsigned char c);
typedef bool  (__fastcall* fn_scumwarn)(void* pause, void* on_quit);
typedef bool  (__fastcall* fn_trydepart)(void* box);
typedef bool  (__fastcall* fn_iskitten)(void* cat);
typedef void* (__fastcall* fn_loadchar)(void* chr, void* name, uint8_t a, uint8_t b);
typedef char  (__fastcall* fn_this_c)(void* self);

fn_this       o_InitSystems = nullptr;
fn_this       o_NextTurn    = nullptr;
fn_two_ptr    o_GetChoice   = nullptr;
fn_this_ptr_b o_DoAction    = nullptr;
fn_this_ptr   o_Trigger     = nullptr;
fn_this_i32   o_BeginTurn   = nullptr;
fn_this       o_EndTurn     = nullptr;
fn_this       o_FrameBegin  = nullptr;
fn_this_ptr   o_ApplyAction = nullptr;
fn_this_ptr   o_QueueDecision = nullptr;
fn_ptr_i32    o_SaveScum      = nullptr;
fn_scumwarn   o_ScumWarn      = nullptr;
fn_trydepart  o_TryDepart     = nullptr;
fn_iskitten   o_IsKitten      = nullptr;
fn_loadchar   o_LoadChar      = nullptr;
fn_this       o_EndRunDefeat  = nullptr;
fn_this       o_GenerateMap   = nullptr;   // MapScreen::generate_map
typedef void* (__fastcall* fn_classes_t)(void* save, void* out, bool with_colorless);
fn_classes_t o_UnlockedClasses = nullptr;     // MewSaveFile's class-list function: what every "any unlocked class" ability pool is built from
fn_unlock_chk o_IsAbility = nullptr, o_IsPassive = nullptr, o_IsItem = nullptr, o_IsLevel = nullptr, o_IsBoss = nullptr;   // MewSaveFile "is X unlocked" checks
fn_prop_int    o_PropGetInt    = nullptr;   // the save-properties getter (key BY VALUE)
fn_this       o_EquipDone     = nullptr;   // the gear screen's done-closure (std::function body, void())
fn_two_ptr    o_GainCat       = nullptr;   // the event effect gain_cat_familiar(context, <unused>)
fn_this       o_TimeDelay     = nullptr;
fn_this       o_MapUpdate     = nullptr;
fn_this_ptr   o_EnterNode     = nullptr;
fn_this       o_StatusMenu    = nullptr;
fn_this       o_SaveSelUpdate = nullptr;
fn_ptr_i32_b  o_SaveSlotClick = nullptr;
fn_two_ptr    o_MewDirInit    = nullptr;
fn_this       o_EventChoice   = nullptr;   // sub_140937F30(capture*)
fn_this       o_EventUpdate   = nullptr;
fn_this_ptr   o_LevelSelect   = nullptr;   // select_option(this, LevelUpOption*)
fn_this       o_LevelUpdate   = nullptr;
fn_this       o_CombatMenu    = nullptr;
fn_this       o_ButtonUpdate  = nullptr;
fn_this       o_EndRunFinalize = nullptr;
fn_this       o_TryAbandon     = nullptr;
fn_this       o_EquipmentClick = nullptr;
fn_this_i32   o_SelectAct     = nullptr;
fn_this       o_ChapterLower  = nullptr;
fn_this       o_ChapterRaise  = nullptr;
// Two pointers, not one: UpdateDecision RETURNS a TurnAction by value, so the
// caller passes the sret buffer in rdx (`lea rdx, [sret]` @ 0x1408DF2EF, one
// instruction before the call). Typed as a one-argument function, the detour
// clobbers rdx before the trampoline runs and the game writes 0x88 bytes --
// a whole TurnAction, std::function and all -- over whatever rdx happened to
// hold. Same ABI as GetChoice, and for the same reason.
fn_two_ptr    o_UpdateDecision = nullptr;
fn_this_c     o_HighlightRefresh = nullptr;

// MewSaveFile::Store / ::Load. Both are (this, std::string key BY VALUE,
// ByteStream&); the return value is the key destructor's and no caller reads
// it, which is what makes suppressing them possible at all.
typedef void* (__fastcall* fn_three_ptr)(void* a, void* b, void* c);
fn_three_ptr  o_SFStoreBlob   = nullptr;
fn_three_ptr  o_SFLoadBlob    = nullptr;

// TurnControl+0x60 is the live count of queued decisions, read off the drain
// loop in TurnControl::update (ring at +0x48, capacity at +0x50, head at +0x58,
// count at +0x60). Recorded rather than derived because a queue that grows in
// one run and not another is the cheapest desync signal we have.
uint32_t queue_depth(const void* turn_control) {
    if (!turn_control) return 0;
    uint64_t n = 0;
    if (!mem_read((const uint8_t*)turn_control + 0x60, &n, sizeof(n))) return 0;
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}

// ---- shared formatting helpers -------------------------------------------

// "cls=glaiel::MoveAbility" plus, when pointers=1, " this=00007FF6..."
struct Who {
    char text[320];
    Who(const void* obj, const char* field) {
        char cls[192];
        rtti_class_name(obj, cls, sizeof(cls));
        if (tune::kPointers)
            _snprintf_s(text, sizeof(text), _TRUNCATE, "cls=%s %s=%p", cls, field, obj);
        else
            _snprintf_s(text, sizeof(text), _TRUNCATE, "cls=%s", cls);
    }
};

struct TaDump {
    char text[1200];
    explicit TaDump(const void* ta) {
        format_turn_action(text, sizeof(text), ta, tune::kTaRaw);
    }
};

// ---- hooks ---------------------------------------------------------------

void __fastcall h_InitSystems(void* self) {
    log_line("INIT", "ApplicationBase::initSystems enter");
    o_InitSystems(self);
    log_line("INIT", "ApplicationBase::initSystems leave");
}

void __fastcall h_NextTurn(void* self) {
    uint32_t t = log_bump_turn();
    log_stage_quiet("battle: turn %u begins", t);

    // THE RE PROBE GETS THE POINTER HERE, ABOVE THE SESSION GATE.
    //
    // This is the only place a TurnControl* is ever in hand, and it was being
    // handed over INSIDE lockstep's own `if (lockstep_active())` -- so a
    // single-player run, which has no session by definition, never handed it over
    // at all. The probe then sits armed-but-blind for the whole session while its
    // log says nothing (2026-09-21: two play sessions, zero hits, and the reason
    // was this gate, twice -- first the guard inside lockstep_turn_boundary, then
    // the call to it). The probe needs a POINTER, not a session.
    listprobe_set_turn_control(self);

    // `sub`, and with it the two tables the battle list lives in, is reached
    // through the same pointer -- so the run editor is handed it in the same
    // place, above the same session gate.
    roster_set_turn_control(self);

    // And lockstep, for the same reason again (2026-09-22): its debug sweep needs the LIVE
    // character list when there is no snapshot to sweep -- a single-player fight, or the
    // solo half of "continue alone". Handed over here rather than inside
    // lockstep_turn_boundary, which the `if (lockstep_active())` below never enters
    // without a session.
    lockstep_set_turn_control(self);

    // The unit spawn / transform / remove experiment (ui.dev_tools, single player): a panel button only requests it, and it
    // runs here -- on the game thread, at the boundary the board sync uses -- then follows the units it touched.
    spawntest_turn_boundary(self);

    // Before the original: snapshot the roster on the first boundary of a
    // battle, then exchange this turn's hash while the queue is still the one
    // the turn ended with.
    if (lockstep_active()) { lockstep_turn_boundary(self); lockstep_reseed(nullptr, 0); }
    Who      w(self, "tc");
    log_line("NEXTTURN", ">>> turn %u begins  %s", t, w.text);

    // Also the flush point for the binary stream: a turn boundary is the one
    // place in a turn-based game where a few hundred microseconds of file I/O
    // cannot be mistaken for a frame-pacing effect.
    uint64_t total = 0, global = 0;
    rng_counters(&total, &global);
    record_turn(t, self, total, global);
    if (record_active()) {
        uint32_t distinct = 0; bool overflow = false;
        uint64_t st = 0, hp = 0, im = 0, tl = 0;
        record_stream_stats(&distinct, &overflow, &st, &hp, &im, &tl);
        uint32_t unknown = record_unknown_slots();
        log_line("RNG", "turn %u: %llu draws | tls+0x178=%llu other_tls=%llu"
                        " stack=%llu heap=%llu image=%llu | %u distinct streams%s%s",
                 t, (unsigned long long)total, (unsigned long long)global,
                 (unsigned long long)tl, (unsigned long long)st,
                 (unsigned long long)hp, (unsigned long long)im, distinct,
                 overflow ? " (TABLE OVERFLOWED)" : "",
                 unknown ? " (UNRESOLVED ABILITY SLOTS)" : "");
        if (unknown)
            log_line("SLOT", "%u action(s) so far named an ability the actor does not"
                             " own -- replay cannot reproduce those", unknown);
    }

    o_NextTurn(self);
    log_line("NEXTTURN", "<<< turn %u dispatched", t);
}

void* __fastcall h_GetChoice(void* self, void* out) {
    void* ret = o_GetChoice(self, out);

    // --- run B: inject the recorded decision -----------------------------
    //
    // The original still runs first. It is a poll with no RNG of its own
    // (Brain::GetChoice has zero TLS loads), but it does its own bookkeeping,
    // and overwriting its result is a smaller intervention than skipping it.
    //
    // Two cases, and the second matters as much as the first:
    //   - we have a decision to inject -> overwrite the buffer with it;
    //   - we have one outstanding but not yet applied -> force type=1, so a
    //     stray click during replay cannot queue a second decision on top of
    //     the one already in flight.
    // Lockstep and replay are alternatives, never both: one injects from a
    // socket, the other from a file, and they would fight over the same buffer.
    if (lockstep_active()) {
        if (lockstep_fill_choice(self, ret)) return ret;
    } else if (replay_active()) {
        if (replay_fill_choice(self, ret)) {
            TaDump d(ret);
            log_line("REPLAY", "injected %s", d.text);
            return ret;
        }
        if (replay_outstanding()) {
            uint32_t none = TA_None;
            mem_write(ret, &none, sizeof(none));
        }
    }

    // Suppress the poll. Without this a single tutorial battle produced 3422
    // CHOICE lines against 12 real actions.
    static LONG volatile polls = 0;
    TurnAction a{};
    bool       decided = mem_read(ret, &a, sizeof(a)) && a.type != TA_None;

    if (!decided && !tune::kChoiceAll) {
        InterlockedIncrement(&polls);
        return ret;
    }

    LONG suppressed = InterlockedExchange(&polls, 0);
    Who    w(self, "brain");
    TaDump d(ret);
    if (suppressed)
        log_line("CHOICE", "%s %s (after %ld polls)", w.text, d.text, suppressed);
    else
        log_line("CHOICE", "%s %s", w.text, d.text);
    return ret;
}

void __fastcall h_DoAction(void* self, void* ta, unsigned char flag) {
    Who    w(self, "char");
    TaDump d(ta);
    log_line("DOACTION", "%s flag=%u %s", w.text, (unsigned)flag, d.text);
    o_DoAction(self, ta, flag);
}

void __fastcall h_Trigger(void* self, void* ta) {
    Who    w(self, "abil");
    TaDump d(ta);
    log_line("TRIGGER", "%s %s", w.text, d.text);
    o_Trigger(self, ta);
}

void __fastcall h_BeginTurn(void* self, int arg) {
    Who w(self, "char");
    log_line("BEGINTURN", "%s arg=%d", w.text, arg);
    lockstep_reseed(self, arg);
    o_BeginTurn(self, arg);
}

void __fastcall h_EndTurn(void* self) {
    Who w(self, "char");
    log_line("ENDTURN", "%s", w.text);
    o_EndTurn(self);
    lockstep_after_endturn(self);
}

void __fastcall h_ApplyAction(void* self, void* ta) {
    TaDump d(ta);
    log_line("APPLY", "%s", d.text);

    // A DUPLICATE-ACTION PROBE (2026-09-23). Measured on the second battle of a run whose node
    // entry AGREED (rng and cats): at turn 0 the CLIENT's log carries an invoke+ability pair
    // TWICE -- same actor, same target (0,4), same direction (0,-1), two DIFFERENT Ability
    // pointers -- where the host carries it once. That one extra cast is what leaves two of the
    // four human cats dead on that peer (hp 5|0, 1|0) and what moves its rng, and it is the whole
    // remaining difference: the rosters now match index for index.
    //
    // The class names in the APPLY line cannot separate the two readings that matter here:
    // "the same actor ran the same ability twice" and "two distinct ability objects were run".
    // Counting applies per turn and per (actor, ability class) separates them, and it is the
    // question that decides the fix -- a doubled apply is a lockstep/queue problem, a second
    // object is a data problem (the per-peer cat apply creating one).
    {
        static uint32_t s_turn = 0xFFFFFFFFu;
        struct Seen { const void* actor; char cls[64]; };
        static Seen     s_seen[24];
        static uint32_t s_n = 0;
        const uint32_t  turn = log_turn();
        if (turn != s_turn) { s_turn = turn; s_n = 0; }

        TurnAction a{};
        if (mem_read(ta, &a, sizeof(a)) && a.type != TA_None) {
            const void* actor = a.actor ? a.actor : current_actor(self);
            char cls[64] = {};
            rtti_class_name(a.ability, cls, sizeof(cls));
            uint32_t dup = 0;
            for (uint32_t i = 0; i < s_n; ++i)
                if (s_seen[i].actor == actor && strcmp(s_seen[i].cls, cls) == 0) ++dup;
            if (s_n < 24) {
                s_seen[s_n].actor = actor;
                strncpy_s(s_seen[s_n].cls, sizeof(s_seen[s_n].cls), cls, _TRUNCATE);
                ++s_n;
            }
            // AND WHO CALLED IT (2026-09-23). The doubled application is local -- measured in a
            // battle where NO action travelled on the wire at all, and where both peers' queues
            // read empty in the same hash -- so one decision is being APPLIED twice rather than
            // queued twice. The two calls must therefore come from two different places, and the
            // caller's return address names them without another theory.
            const uintptr_t ret = (uintptr_t)_ReturnAddress();

            // AND WHAT THAT ACTOR HOLDS (2026-09-23). Everything above narrowed the doubled
            // action to one cat: the one whose data changed, i.e. the cat the draw picked for a
            // level-up -- the user saw it, and the log agrees (the duplicated actor is index 7,
            // cat 024d, the drawn cat). The cat-data path is now de-duplicated (33 applies -> 9),
            // so the remaining question is what the FIRST write of genuinely-changed bytes left
            // behind on the live cat. mgmp_ability.h has the layout: Character+0xD0 move, +0xD8
            // attack, +0xE0 bonus, +0xEC spell count, +0xF0 spell array. If the same ability is
            // listed twice there -- say once as a bonus and once as a spell -- then the extra
            // application is explained and the fix belongs in how the data is applied, not in the
            // battle code. Read-only, and bounded: four slots, and it only prints on a duplicate.
            char ab[320] = {};
            int  ao = 0;
            {
                struct { size_t off; const char* name; } spots[3] = {
                    { kCharAbilMove, "move" }, { kCharAbilAttack, "atk" }, { kCharAbilBonus, "bonus" },
                };
                const uint8_t* ch = (const uint8_t*)actor;
                for (uint32_t s = 0; s < 3 && ao < (int)sizeof(ab) - 40; ++s) {
                    void* p = nullptr;
                    if (!mem_read(ch + spots[s].off, &p, sizeof(p)) || !p) continue;
                    char n[64] = {};
                    ability_gon_name(p, n, sizeof(n));
                    ao += _snprintf_s(ab + ao, sizeof(ab) - ao, _TRUNCATE, " %s=0x%llX('%s')",
                                      spots[s].name, (unsigned long long)(uintptr_t)p, n);
                }
                uint32_t n_sp = 0;
                const void** sp = nullptr;
                if (mem_read(ch + kCharSpellCount, &n_sp, sizeof(n_sp)) && n_sp <= 0x40 &&
                    mem_read(ch + kCharSpellData, &sp, sizeof(sp)) && sp) {
                    ao += _snprintf_s(ab + ao, sizeof(ab) - ao, _TRUNCATE, " spells=%u[", n_sp);
                    for (uint32_t s = 0; s < n_sp && s < 4 && ao < (int)sizeof(ab) - 40; ++s) {
                        void* p = nullptr;
                        if (!mem_read(sp + s, &p, sizeof(p)) || !p) { ao += _snprintf_s(ab + ao, sizeof(ab) - ao, _TRUNCATE, " -"); continue; }
                        char n[64] = {};
                        ability_gon_name(p, n, sizeof(n));
                        ao += _snprintf_s(ab + ao, sizeof(ab) - ao, _TRUNCATE, " 0x%llX('%s')",
                                          (unsigned long long)(uintptr_t)p, n);
                    }
                    ao += _snprintf_s(ab + ao, sizeof(ab) - ao, _TRUNCATE, " ]");
                }
            }

            log_line("APPLY", "  ^ turn %u: actor 0x%llX is on apply #%u of %s this turn%s"
                              "  [caller rva=0x%llX]%s",
                     turn, (unsigned long long)(uintptr_t)actor, dup + 1, cls,
                     dup ? "   <<<< DUPLICATE: the same actor ran this class twice in one turn"
                         : "",
                     (unsigned long long)(ret - (uintptr_t)GetModuleHandleA(nullptr)),
                     dup ? ab : "");
        }
    }

    if (record_active()) {
        TurnAction a{};
        if (mem_read(ta, &a, sizeof(a))) {
            EvAction e{};
            e.turn        = log_turn();
            e.type        = a.type;
            e.target_x    = a.target_x;
            e.target_y    = a.target_y;
            e.dir_x       = a.dir_x;
            e.dir_y       = a.dir_y;
            e.ability_ptr = (uint64_t)a.ability;
            e.actor_ptr   = (uint64_t)a.actor;
            e.ability_cls = record_intern_class(a.ability);
            e.actor_cls   = record_intern_class(a.actor);
            e.queue_depth = queue_depth(self);
            e.b30         = a.tail[0x30 - 0x28];
            e.b31         = a.tail[0x31 - 0x28];

            // The identity the replayer will actually use. TurnAction+0x20
            // arrives NULL from the brain -- Character::DoAction fills it in,
            // and that runs *after* ApplyTurnAction -- so the actor has to come
            // from TurnControl itself here. Prefer the action's own actor when
            // it happens to be set (type 6 reaction broadcasts carry one).
            const void* actor = a.actor ? a.actor : current_actor(self);
            AbilitySlot slot  = ability_slot_of(actor, a.ability);
            e.slot_kind  = slot.kind;
            e.slot_index = slot.index;
            if (slot.kind == SLOT_UNKNOWN) record_note_unknown_slot();

            char gon[64];
            if (ability_gon_name(a.ability, gon, sizeof(gon)))
                e.ability_name = record_intern_name(gon);

            // Character+0x68 is the Brain (TurnControl::update reads exactly
            // this to call UpdateDecision). Its class separates the human's
            // decisions from the AI's, and only the human's are replayable.
            const void* brain = nullptr;
            if (actor && mem_read((const uint8_t*)actor + 0x68, &brain, sizeof(brain)))
                e.brain_cls = record_intern_class(brain);

            // +0x04 is deliberately absent: it is uninitialized padding and
            // would differ between runs for no reason. See mgmp_turnaction.h.
            record_action(e);
        }
    }

    // Advance and validate the replay FIFO. Done before the original runs, so
    // the comparison sees the action exactly as it was recorded -- ApplyTurnAction
    // mutates the slot at TurnControl+0x90 on its way through.
    if (replay_active())   replay_on_applied(ta, current_actor(self));
    if (lockstep_active()) lockstep_on_applied(ta, current_actor(self));

    rng_ledger_phase(true);       // the draws the game's own apply makes are one record (the RNG ledger, mgmp_diag), the ones between actions another
    o_ApplyAction(self, ta);
    rng_ledger_phase(false);
}

// The push side of the ring. Fires for every deferred reaction a passive or
// status queues, which is where types 6 and 7 come from -- neither ever
// originates in a brain, so neither goes on the wire, but both are derived from
// proc rolls and so both diverge the instant an RNG stream does.
void __fastcall h_QueueDecision(void* self, void* ta) {
    // SAY WHO PUSHES WHAT (2026-09-23). This is the last step of a long narrowing: the one
    // remaining divergence between the peers is an action that is APPLIED twice on the client,
    // and the offline disassembly says why the apply side cannot be it -- ApplyTurnAction has
    // exactly one caller, the big turn-layer body at 0x8E2A30, and the apply that duplicates is
    // inside a LOOP over the queue (`cmp ebx,0x64 / jl`). Two applies therefore mean two QUEUE
    // ENTRIES, and this is the push side of that ring: `QueueDecision` is where a passive or a
    // status defers a reaction, which is also where types 6 and 7 come from (neither ever
    // originates in a brain, and the null-ability invoke that duplicates is exactly a type 7).
    //
    // Type 1 is the brain's per-frame "nothing yet" poll and never reaches the ring, so it is
    // skipped: 3787 of them against 30 real pushes in one recorded run. Everything else is
    // printed with the caller's return address, which is what separates "the same producer pushed
    // twice" from "two different producers pushed once" -- the two answers have different fixes.
    if (ta) {
        uint32_t type = 0;
        if (mem_read(ta, &type, sizeof(type)) && type >= 2) {
            TaDump d(ta);
            // AND WHO CALLED THE CALLER (2026-09-23). The offline disassembly of the producer
            // settled the shape: both pushes come from the SAME call site (return address
            // 0x26E638, inside the function at 0x26E550, calling QueueDecision at 0x8E3870), and
            // they carry the SAME invoke object, byte for byte. So this is not two environment
            // objects -- it is ONE object processed twice. The two readings left are "the list it
            // sits in holds it twice" and "the update that walks that list ran twice", and the
            // frames above the producer separate them: a repeated frame above the same walker
            // means the walker ran twice, while the same walker reaching the same object at two
            // different list positions shows up as identical nearby frames with a different
            // caller for each.
            void*  frames[8] = {};
            USHORT n = RtlCaptureStackBackTrace(0, 8, frames, nullptr);
            char   bt[320] = {};
            int    bo = 0;
            for (USHORT i = 1; i < n && bo < (int)sizeof(bt) - 24; ++i)
                bo += _snprintf_s(bt + bo, sizeof(bt) - bo, _TRUNCATE, " 0x%llX",
                                  (unsigned long long)((uintptr_t)frames[i] - g_base));
            // ★ AND THE OBJECT THE WALK IS HOLDING (2026-09-23). The offline disassembly of the
            // walker (0x96A6ED, loop at 0x96ABC0..0x96AC35) shows each entry carries an object in
            // rbx with a "handled this turn" byte at +0x11, set only AFTER the virtual call at
            // [rax+0x30] -- the call that ends up here. So a second push from the same call site
            // cannot be the same object twice: the flag would have stopped it. That leaves two
            // objects in the walk, each holding the SAME std::function (which is why the invoke
            // pointer above is byte-identical between the two pushes). rbx is callee-saved, so
            // reading it here names which of the two it is -- same value twice means the walk ran
            // twice anyway, different values mean two environment objects share one effect and the
            // extra one is what has to be found (and it will be, in whatever list feeds the walk).
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_INTEGER;
            RtlCaptureContext(&ctx);
            log_line_lvl(LogLevel::Warn, "QUEUE",
                         "!! push turn %u: %s  [depth_now=%u obj=0x%llX] back: %s",
                         log_turn(), d.text, (unsigned)queue_depth(self),
                         (unsigned long long)ctx.Rbx, bt);
        }
    }

    if (record_active()) {
        uint32_t type = 0;
        if (mem_read(ta, &type, sizeof(type)) && type > TA_None) {
            // type == 1 is the brain's "nothing decided yet" poll. QueueDecision
            // rejects it internally, so it never reaches the ring -- but the call
            // still happens, once per frame, for as long as the player is
            // thinking. Run A recorded 3787 of them against 30 real pushes.
            //
            // They are not just volume: the count is a function of how long a
            // human took to move and of the frame rate, so it differs between
            // any two runs by construction. Recording them would make every
            // diff fail on the first turn for a reason that means nothing.
            EvQueue q{};
            q.turn = log_turn();
            q.type = type;
            // Depth is sampled before the push, so +1 is what the ring will
            // hold once the original returns.
            q.depth_after = queue_depth(self) + 1;
            uintptr_t ret = (uintptr_t)_ReturnAddress();
            q.site = (ret >= g_base && (ret - g_base) <= 0xFFFFFFFFull)
                         ? (uint32_t)(ret - g_base) : 0;
            record_queue(q);
        }
    }
    o_QueueDecision(self, ta);
}

// The one hook that changes the game instead of watching it.
//
// sub_1408DD9C0 is the save-scum penalty: it increments the scum counter at
// RunState+0xE0, walks the cat roster applying per-cat penalties, and queues
// the Steven NPC scripts. Swallowing the call removes all three.
//
// This exists for the capture methodology, not for convenience. Repeating a
// battle means reloading the same save repeatedly, and the penalty MUTATES CAT
// STATE AS A FUNCTION OF HOW MANY TIMES YOU RELOADED -- so with it live, run N
// and run N+1 start from different states by construction and no A/B capture
// can be trusted. It is a determinism hazard for the instrument, in exactly the
// way an unfenced RNG stream is.
//
// It is off by default. A capture taken with `hook_savescum = 1` is not a
// capture of the shipped game, so the banner says so out loud.
// ---------------------------------------------------------------------------
// hook_timedelay -- convert TimeDelayStatusApplication from a wall-clock
// countdown to a turn countdown.
//
// This is the one place in the battle sim measured to advance on real time.
// Slot 11 does, per update:
//
//     [this+0xF8] -= dt * this->rate * scene->timescale;
//     if ([this+0xF8] < 0) apply_the_statuses();
//
// and does nothing whatsoever when the countdown is still positive (the
// not-expired branch jumps straight to the epilogue -- verified, it is
// `add rsp,0B0h / pop x5 / retn`). So the whole function is that one statement,
// which is what makes it safe to intercept: when the effect is not due we can
// simply not call the original, and when it is due we hand control back so the
// REAL status-application code runs, untouched.
//
// Why it has to change at all: two peers in turn-lockstep run at different
// frame rates -- run E measured the same battle at 23,211 frames and at 12,230,
// with turns lasting 2.6x to 5.4x longer on one side. Three seconds of dt
// therefore spans a different number of TURNS on each peer, so the AstroZombie's
// delayed Cleanse+FullHeal lands before one peer's next action and after the
// other's. Everything downstream of that desyncs.
//
// The conversion: wait `timedelay_turns` turn boundaries instead of N seconds.
// A turn boundary is the coarsest thing both peers already agree on by
// construction, and the shipped delays (.1, .25, 1.13333, 3 seconds) are all
// shorter than a turn, so "the next turn boundary" is the nearest deterministic
// reading of what the data was asking for.
//
// The branch logic lives in mgmp_timedelay.h as a pure function so it can be
// tested without a running game -- the only ordinarily-reachable content that
// exercises this path is one miniboss. Everything below is plumbing.

void __fastcall h_TimeDelay(void* self) {
    if (!self) { if (o_TimeDelay) o_TimeDelay(self); return; }

    double v = 0.0;
    if (!mem_read((const uint8_t*)self + 0xF8, &v, sizeof(v))) {
        if (o_TimeDelay) o_TimeDelay(self);          // unreadable: do no harm
        return;
    }

    const uint32_t turn = log_turn();
    const TdDecision d  = td_decide(v, turn, tune::kTimeDelayTurns);

    switch (d.action) {
    case TD_WAIT:
        return;                                       // the original would only
                                                      // have decremented
    case TD_PASSTHROUGH:
        if (o_TimeDelay) o_TimeDelay(self);
        return;

    case TD_TAKE_OVER:
        if (!mem_write((uint8_t*)self + 0xF8, &d.value, sizeof(d.value))) {
            if (o_TimeDelay) o_TimeDelay(self);       // could not take over
            return;
        }
        log_line("TDELAY", "converted %.5fs -> %u turn(s): turn %u, due %u, status=%p",
                 v, tune::kTimeDelayTurns, turn, d.due, self);
        return;

    case TD_FIRE:
        mem_write((uint8_t*)self + 0xF8, &d.value, sizeof(d.value));
        log_line("TDELAY", "firing at turn %u (due %u) status=%p", turn, d.due, self);
        if (o_TimeDelay) o_TimeDelay(self);
        return;
    }
}

// MULTIPLAYER ONLY (2026-10-01, the user's rule): "disable the save-scum penalty and its warnings in
// multiplayer; single player keeps them". A session with another player in it is multiplayer; anything
// else -- no room, a room with nobody connected, a player away on its house boss -- is single player and
// gets the shipped game.
static bool multiplayer_live() { return net_active() && net_peer_count() >= 2; }

void __fastcall h_SaveScum(void* run, int mode) {
    if (!multiplayer_live()) { o_SaveScum(run, mode); return; }
    static LONG volatile swallowed = 0;
    LONG n = InterlockedIncrement(&swallowed);
    uint32_t counter = 0;
    if (run) mem_read((const uint8_t*)run + 0xE0, &counter, sizeof(counter));
    log_line("SCUM", "swallowed save-scum penalty #%ld (mode=%d, counter stays at %u)",
             n, mode, counter);
    // Deliberately does NOT call o_SaveScum.
}

// Steven's warning on the way out of a battle. In multiplayer the game is told, for the length of this one
// call, that quitting does not count -- it then quits at once, writes no `savescumlocation`, and the next
// load has nothing to punish. The flag is put back whatever happens.
bool __fastcall h_ScumWarn(void* pause, void* on_quit) {
    if (!multiplayer_live()) return o_ScumWarn(pause, on_quit);
    const uintptr_t at = addr_of_data(D_MewDirectorPtr);
    uint8_t* dir = nullptr;
    if (!at || !mem_read((const void*)at, &dir, sizeof(dir)) || !dir) return o_ScumWarn(pause, on_quit);
    uint8_t armed = 0;
    if (!mem_read(dir + kDir_ScumArmed, &armed, 1) || !armed) return o_ScumWarn(pause, on_quit);
    const uint8_t off = 0;
    mem_write(dir + kDir_ScumArmed, &off, 1);
    log_line("SCUM", "multiplayer: quitting mid-battle without Steven's warning (and without a penalty on reload)");
    const bool r = o_ScumWarn(pause, on_quit);
    // The quit's own transition is deferred, so the director is still the same object here.
    uint8_t* dir_now = nullptr;
    if (mem_read((const void*)at, &dir_now, sizeof(dir_now)) && dir_now == dir) mem_write(dir + kDir_ScumArmed, &armed, 1);
    return r;
}

// A KITTEN CANNOT TAKE THE FIELD, so once the run is under way nobody is one (2026-10-01, the user's rule).
// Whether a cat is a kitten is decided by the CURRENT DAY of the player's own save (see T_IsKitten), and each
// player's save has its own day: in the 3-player halt the same cat was a kitten on one peer (every stat -2, 8 HP
// less) and an adult on the others. The game never lets a kitten into a party, so outside the preparation
// screens -- on the map, in a battle -- the answer is simply no, on every peer alike. In the warehouse, the
// collar / gear / chapter pages (where the player is still choosing, and the game shows who is a kitten) the
// game's own answer stands. Only in a room: alone, the shipped game is untouched.
//
// A SESSION CAT IS NEVER A KITTEN (2026-10-02). The page gate alone failed: on the map the client's page detector read '选择装备', the game
// asked about the host's three cats while building the battle, and they were kittens there -- every stat -2, SPEED 5/4/4 became 3/2/2 and
// the party was placed in another order. A session copy (id 0x70000000..0x7FFFFFFF) only exists for a cat in the run's party, so in a room
// it is answered "no" whatever the page; the player's own cats keep the page rule.
bool __fastcall h_IsKitten(void* cat) {
    if (net_active() && net_peer_count() >= 2) {
        uint64_t id = 0;
        if (cat && mem_read((const uint8_t*)cat + kCatData_SaveId, &id, sizeof(id)) && id >= 0x70000000ull && id < 0x80000000ull) return false;
        const PageState pg = page_self();
        if (pg == PageState::InGame) return false;
        // A CAT IN THE PARTY is no kitten on the preparation pages either (2026-10-03): the House's box had to accept it to put it there, and a copy the older builds made of a clone (or any cat whose
        // birth day sits within a day of the save's) answered "kitten" there and lost 2 of every stat on the collar page. A cat outside the party keeps the game's own answer.
        if ((pg == PageState::Collar || pg == PageState::Equipment || pg == PageState::Chapter) && id && catsync_in_run_party(id)) return false;
    }
    return o_IsKitten(cat);
}

// ENEMY DURABILITY (2026-10-01): in a room every enemy's health, maximum health and starting armor are
// doubled (mgmp_balance.h). The definition loader runs once per character and every phase of a boss is its own
// definition built through the same factory, so this one place covers them all. Every peer runs it with the same
// inputs, so the lockstep state stays identical. Alone, the shipped game is untouched.
void* __fastcall h_LoadChar(void* chr, void* name, uint8_t a, uint8_t b) {
    if (!multiplayer_live() || !chr) return o_LoadChar(chr, name, a, b);
    // The shared stream before every definition a battle loads: the two peers' lists show the first load after which they drew differently.
    if (const uint64_t* s = rng_global_stream()) {
        char what[64] = "?";
        mem_read_std_string(name, what, sizeof(what));
        uint64_t s0 = 0;
        mem_read(s, &s0, sizeof(s0));
        log_line("RNGTRACE", "stream %016llx before loading '%s'", (unsigned long long)s0, what);
    }
    unlocks_build_load();   // inside the battle build: this load starts from a stream every peer shares
    {   char nm[48] = "?"; mem_read_std_string(name, nm, sizeof(nm)); unlocks_midbattle_load(nm); }   // in a fight: the same fence for a summon or a pickup (what it draws stays inside it)
    int32_t hp_before = 0;
    mem_read((const uint8_t*)chr + kChr_Hp, &hp_before, sizeof(hp_before));
    void* r = o_LoadChar(chr, name, a, b);
    balance_scale_enemy(chr, hp_before);
    unlocks_build_load_done();   // ...and again when it is over: what the definition drew (a random cat's name) must not reach the next roll
    unlocks_midbattle_load_done();
    return r;
}

// The House's departure. In a 3- or 4-player room a party over the per-player limit does not set off; the
// press is reported as handled so the shared door handler does not end the day instead. See T_TryDepart.
// The main-story item the cat with this id already wears, if any -- asked of the game's own predicate through
// the equipment guard. Unreadable answers "no" (and logs): a departure must not hang on a failed read.
static bool cat_carries_story_item(uint64_t id, char* item, size_t cap) {
    __try {
        const uintptr_t at = addr_of_data(D_MewDirectorPtr);
        auto by_id = (void* (__fastcall*)(void*, uint64_t))addr_of_call(C_CatDataById);
        auto legacy = (EquipmentLegacy)addr_of_call(C_EquipmentLegacyQuest);
        uint8_t* dir = nullptr; void* registry = nullptr;
        if (!at || !by_id || !legacy || !mem_read((const void*)at, &dir, sizeof(dir)) || !dir ||
            !mem_read(dir + kDir_CatRegistry, &registry, sizeof(registry)) || !registry) return false;
        void* cat = by_id(registry, id);
        return cat && cat_wears_story_item(cat, legacy, item, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line_lvl(LogLevel::Warn, "SETUP", "story-item check of cat %llx faulted -- not blocking the departure", (unsigned long long)id);
        return false;
    }
}

bool __fastcall h_TryDepart(void* box) {
    uint32_t n = 0;
    uint64_t ids[kDepart_SlotCnt] = {};
    const uint8_t* slots = nullptr;
    if (box && mem_read((const uint8_t*)box + kDepart_Slots, &slots, sizeof(slots)) && slots) {
        for (uint32_t i = 0; i < kDepart_SlotCnt; ++i) {
            void* slot = nullptr;
            if (mem_read(slots + i * sizeof(void*), &slot, sizeof(slot)) && slot) {
                uint64_t id = 0;
                if (mem_read((const uint8_t*)slot + 0x80, &id, sizeof(id)) && id) ids[n] = id;
                ++n;
            }
        }
    }
    {   // the departure, for the log of the preparation stage: who is in the box and what the room said
        char list[200] = {}; int w = 0;
        for (uint32_t i = 0; i < n && i < kDepart_SlotCnt && w >= 0 && (size_t)w < sizeof(list) - 24; ++i)
            w += _snprintf_s(list + w, sizeof(list) - w, _TRUNCATE, " %llx", (unsigned long long)ids[i]);
        SYSTEMTIME st{}; GetLocalTime(&st);
        log_line("DEPART", "[%02d:%02d:%02d] the House's box sets off with %u cat(s):%s", st.wHour, st.wMinute, st.wSecond, n, list);
    }
    if (!room_depart_allowed(n)) { log_line("DEPART", "refused by the room (party size rule)"); return true; }
    catsync_log_snapshot("departure (the box is accepted)");
    // A client may not bring a main-story item (the host alone carries those): the click gate stops equipping one,
    // this stops departing with a cat that already wears one. Only while the client is still preparing.
    if (setup_client_preparing()) {
        for (uint32_t i = 0; i < n && i < kDepart_SlotCnt; ++i) {
            char item[48] = {};
            if (ids[i] && cat_carries_story_item(ids[i], item, sizeof(item)) && !room_refuse_story_item(item)) return true;
        }
    }
    return o_TryDepart(box);
}

// --- phase 5: following the host through the map ---------------------------

void __fastcall h_EnterNode(void* self, void* node) {
    log_stage_quiet("map: a node is being entered");
    lockstep_log_battle_summary("the next node is entered");
    // The client's own clicks are swallowed here; the host's are published.
    // Note there is no "am I replaying this" flag: the injection below calls
    // o_EnterNode, the MinHook trampoline, which bypasses this detour outright.
    bool sent = false;
    if (!follow_on_enter_node(self, node, &sent)) return;
    o_EnterNode(self, node);
    follow_after_enter_node();
}

void __fastcall h_MapUpdate(void* self) {
    o_MapUpdate(self);

    // After the original, not before: the node the host chose should be entered
    // from a map screen that has already finished this frame's update, which is
    // as close as we can get to the native call site (a UI callback fired from
    // inside this same function).
    if (!o_EnterNode) return;
    if (void* node = follow_map_update(self)) {
        o_EnterNode(self, node);
        follow_after_enter_node();
        // READ-ONLY, and off by default: the window dump that finds the field marking "this
        // node is consumed". Called for EVERY followed entry, battle or not, because the
        // difference between those two cases IS the measurement. See follow_probe_node.
        follow_probe_node(node);
        // And the fix that measurement bought: the client's own copy of the map is told the
        // node is done, with the same single-byte write the host's game makes. Client-only
        // and idempotent -- see follow_mark_node_consumed.
        follow_mark_node_consumed(node);
        // And the visual half: the party ICON, by writing the same selection slot the
        // game's own node click writes. See follow_move_marker_to.
        follow_move_marker_to(node);
        // AFTER the original, so EnterNode has already copied this node's seed into
        // the stream -- the perturbation has to land on the state the node will
        // actually draw from, not on the one it is about to replace. No-op on the
        // host, and on any battle node. See follow_perturb_after_enter.
        follow_perturb_after_enter(node);
    }
}

// --- the decision screens --------------------------------------------------
//
// Four hooks, two jobs. The two commit functions both CAPTURE the host's pick
// and SWALLOW the client's -- one splice does both because they are the same
// choke point. The two update ticks exist only so that a choice which arrives
// before this peer's screen is up has somewhere to land.
//
// The commit hooks are the only place in the mod that can DECLINE to run the
// original, and that is the whole point on the client: a click the local player
// made on a screen the host owns must not happen at all.

void __fastcall h_EventChoice(void* cap) {
    if (choice_on_event_commit(cap)) o_EventChoice(cap);
}

void __fastcall h_EventUpdate(void* self) {
    o_EventUpdate(self);
    // After the original, for the same reason h_MapUpdate follows o_MapUpdate:
    // the native click arrives from a UI callback fired inside this update, so
    // injecting here is at the same point in the frame rather than ahead of the
    // screen's own bookkeeping.
    choice_on_event_update(self);
    // ...and the node hash's second sample point. After the choice tick so that
    // an injected choice has already happened: the name at WorldEvent+0x1A10 is
    // written by init and does not change, but sampling last keeps this hook's
    // one ordering rule -- observation follows action -- rather than making the
    // reader work out that it does not matter here.
    nodehash_on_event_screen(self);
}

void __fastcall h_LevelSelect(void* self, void* option) {
    if (choice_on_level_select(self, option)) o_LevelSelect(self, option);
}

void __fastcall h_LevelUpdate(void* self) {
    o_LevelUpdate(self);
    choice_on_level_update(self);
}

// --- peer cursors ----------------------------------------------------------

void __fastcall h_StatusMenu(void* self) {
    o_StatusMenu(self);

    // After the original, for two reasons that happen to point the same way.
    // The hovered tile at StatusMenu+124 is written BY this function, so before
    // it we would be reading last frame's; and the immediate-mode pieces it
    // submits are this frame's, so ours belong in the same batch rather than
    // trailing the previous one.
    cursor_on_status_menu(self);
}

// --- QoL: the combat menu greys out on a cat this peer does not own --------

void __fastcall h_CombatMenu(void* self) {
    // AROUND the original, not after it, and that is the whole point of the
    // pair. combatlock_enter opens a scope that h_ButtonUpdate acts inside; the
    // original is what walks the bar and calls each button's update, which is
    // the only moment between "the game decided this button's state" and "the
    // game drew it". See mgmp_combatlock.h.
    combatlock_enter(self);
    o_CombatMenu(self);
    combatlock_leave();
}

// The status half of the ability highlight. Swallowed while mgmp_aim is drawing
// another player's aim on a cat this peer does not own -- see the block under
// kCalls in mgmp_addresses.h for what it does and what it cost.
//
// Same scoped shape as h_ButtonUpdate below: outside the window this is one
// load and one branch, and the guard is raised for the duration of a single
// call into the game.
char __fastcall h_HighlightRefresh(void* self) {
    if (aim_highlight_suppressed()) return 0;
    return o_HighlightRefresh(self);
}

void __fastcall h_ChapterLower(void* self) {
    if (!setup_client_controls_locked()) o_ChapterLower(self);
}
void __fastcall h_ChapterRaise(void* self) {
    if (!setup_client_controls_locked()) o_ChapterRaise(self);
}

// Lower/raise closures capture the difficulty-record vector, the zero-based
// chapter index and the screen at +0x50/+0x58/+0x60. Verified from both callback
// prologues. Never cache these pointers beyond this live Button::update.
void chapter_control(void* button) {
    if (!tune::kLocalSetup) return;
    char name[64];
    if (!mem_read_std_string((uint8_t*)button + kBtn_Name, name, sizeof(name))) return;
    Target target;
    if (strcmp(name, kBtnName_ChapterLower) == 0) target = T_ChapterLower;
    else if (strcmp(name, kBtnName_ChapterRaise) == 0) target = T_ChapterRaise;
    else return;
    uint8_t *cb = nullptr, *vt = nullptr, *table = nullptr, *record = nullptr;
    void* screen = nullptr;
    uintptr_t invoke = 0;
    int32_t index = -1;
    if (!mem_read((uint8_t*)button + 240, &cb, sizeof(cb)) || !cb ||
        !mem_read(cb, &vt, sizeof(vt)) || !vt ||
        !mem_read(vt + 16, &invoke, sizeof(invoke)) || invoke != addr_of(target) ||
        !mem_read(cb + 0x50, &table, sizeof(table)) || !table ||
        !mem_read(cb + 0x58, &index, sizeof(index)) || index < 0 || index >= 3 ||
        !mem_read(cb + 0x60, &screen, sizeof(screen)) || !screen ||
        !mem_read(table + index * 8, &record, sizeof(record)) || !record) return;
    setup_on_chapter_control(screen, index + 1, (int32_t*)(record + 8), o_SelectAct);
    if (setup_client_controls_locked()) {
        const int32_t disabled = kBtnState_Disabled;
        mem_write((uint8_t*)button + kBtn_State, &disabled, sizeof(disabled));
    }
}

// The home-node callback first drains story scenes (3B52E0), then calls
// 3B5430. That function settles party AND familiars and constructs the House.
// Filter before its first item/cat traversal, never after the warehouse save.
EquipmentGate checked_equipment_gate(void* self) {
    __try {
        return equipment_gate(self, setup_client_preparing(),
            (EquipmentLookup)addr_of_call(C_EquipmentById),
            (EquipmentLocation)addr_of_call(C_EquipmentLocation),
            (EquipmentLegacy)addr_of_call(C_EquipmentLegacyQuest),
            (EquipmentRefresh)addr_of_call(C_EquipmentRefresh));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return EquipmentGate::Unreadable; }
}
void __fastcall h_EquipmentClick(void* self) {
    const auto result=checked_equipment_gate(self);
    if(result!=EquipmentGate::Allow){
        if(result==EquipmentGate::MainStory) room_say_story_only_host();
        log_line_lvl(LogLevel::Warn,"SETUP",result==EquipmentGate::MainStory
            ? "Client preparation: main-story equipment is reserved for the host"
            : "Client equipment click held: cannot safely identify the selected item");
        return;
    }
    o_EquipmentClick(self);
}

// PauseMenu::TryAbandonRun. In a room the press is a proposal, not an abandon -- mgmp_abandon.h.
void __fastcall h_TryAbandon(void* self) {
    if (abandon_on_try()) return;
    o_TryAbandon(self);
}

// What follows EVERY end of a run, won or lost (the abandon / defeat path joined the settlement one in
// 2026-10-01): this player's clones go back to their originals, then the per-run state of every module is reset.
static void run_settled(const char* how, bool was_shared) {
    log_stage("%s: the native end of the run returned (shared run: %s)", how, was_shared ? "yes" : "no");
    lockstep_log_battle_summary(how);
    catsync_log_snapshot("after the native settlement, before the merge");
    if (was_shared) {
        const unsigned merged = catsync_merge_session_cats(how);
        catsync_release_stuck_cats(how);
        const unsigned retired = catsync_retire_peer_copies(how);
        catsync_log_snapshot("after the merge");
        // the settlement saved BEFORE the merge: write the merged state to disk too, so a reload finds it (and the other players' leftover copies, now retired, are not kept out on adventure)
        if (merged || retired) catsync_save_game(how);
    }
    leave_on_settlement();
    checkpoint_clear(true);
    // THE RUN IS OVER, THE SESSION IS NOT (2026-09-28). The same two peers may
    // re-prep in the House and commit the NEXT chapter in this session, so
    // every per-run state a save-screen slot click resets must be reset here
    // too: savefile_on_slot_click does setup_reset_run + the three forgets,
    // and the slot load does choice_reset_run. Without this, setup's g.resume
    // stays true for the whole session and setup_on_select_act silently
    // discards the host's next chapter click -- observed live 2026-09-27
    // 23:55-23:59: the chapter page was up, the click fired no SelectAct,
    // "SETUP done: resume=yes chapter=not seen", and the game never entered
    // the map.
    if (net_active()) {
        setup_reset_run();
        choice_reset_run();
        catsync_forget();
        invsync_forget();
        runhist_forget();
        if (tune::kLocalSetup)
            log_line("SETUP", "run settled in-session; chapter barrier re-armed for the next chapter");
    }
    log_stage("%s: finished -- per-run state reset, the next run starts clean", how);
}

void __fastcall h_EndRunFinalize(void* self) {
    bool filtered = false;
    // A SHARED RUN IS RECOGNISED BY ITS CLONES, not by the network (2026-10-03): when the other player has left or crashed mid-run the session is down or down to one, and the old gate
    // (a live session with a shared roster) then skipped the filter -- the native settlement walked the departed player's clones too and the merge never ran, so this player's originals stayed
    // "out on adventure" for good while the clones and the other player's copies became cats of the House.
    const bool shared = (tune::kLocalSetup && net_active() && setup_has_shared_roster()) || catsync_run_is_shared();
    log_stage("settlement: return home begins (shared run: %s, %u peer(s) connected, net %s)", shared ? "yes" : "no", (unsigned)net_peer_count(), net_active() ? "up" : "down");
    if (shared) {
        if (!catsync_prepare_settlement(self)) {
            // With the other players present the refusal holds the door (a settlement that cannot tell the cats apart must not write a save). With nobody else there is nothing to protect them from,
            // and a door that never opens costs the player the whole run: end it unfiltered, loudly.
            if (net_active() && net_peer_count() >= 2) {
                log_line_lvl(LogLevel::Error, "SETTLE", "!! return-home settlement held: local ownership filter failed or already committed (the reason is in the SETTLE line above)");
                return;
            }
            log_line_lvl(LogLevel::Warn, "SETTLE", "!! return-home settlement: the ownership filter could not be applied and no other player is connected -- ending the run unfiltered");
        } else {
            filtered = true;
        }
    }
    o_EndRunFinalize(self);
    run_settled("settlement", filtered);
}

// THE END OF A RUN THAT WAS LOST OR ABANDONED (2026-10-01, review finding F2). The same walk as the settlement
// above over the same three lists, so the same filter: without it every peer's registry had the OTHER players'
// session copies settled (flagged, their on-adventure bit cleared) too. A filter that cannot be applied does not
// hold this one back -- a run that has ended must be allowed to end -- it falls back to the old, unfiltered walk
// and says so.
void __fastcall h_EndRunDefeat(void* self) {
    bool filtered = false;
    const bool shared = (tune::kLocalSetup && net_active() && setup_has_shared_roster()) || catsync_run_is_shared();
    log_stage("defeat/abandon: the end of a lost run begins (shared run: %s, %u peer(s) connected)", shared ? "yes" : "no", (unsigned)net_peer_count());
    if (shared) {
        filtered = catsync_prepare_settlement(self);
        if (!filtered)
            log_line_lvl(LogLevel::Warn, "SETTLE", "!! defeat/abandon: the ownership filter could not be applied -- ending the run unfiltered");
    }
    o_EndRunDefeat(self);
    run_settled("defeat", filtered);
}

// Post-battle familiar promotion is NOT hooked (removed 2026-09-29). The old
// hook assumed a one-argument function at 0x3B3D60, but that function reads r8
// at entry, and the resolver moved the target to an unrelated RVA in the last
// retests -- skipping it was unsafe and it never fired. The game's promotion is
// allowed to run; roster_normalize_shared() puts every cat back on its owner's
// side afterwards (see mgmp_roster.cpp).

// ui.block_new_cats: every "a new cat joins" event effect does nothing. It has to be the SAME on both peers -- one machine adding a
// cat the other does not would split the rosters -- so the HOST's setting rules: a client takes it from the host's HELLO (session_block_new_cats).
void* __fastcall h_GainCat(void* ctx, void* arg) {
    if (session_block_new_cats()) {
        static bool said = false;
        if (!said) { said = true; log_line("EVENT", "ui.block_new_cats (the host's): the \"a new cat joins\" effect is skipped"); }
        return nullptr;
    }
    return o_GainCat(ctx, arg);
}

// The gear screen's lock. A player with chapter 2 unlocked goes on to the chapter page (the setup barrier lives there); one WITHOUT it
// has no page: the closure starts chapter 1 on the spot. In a room that click is taken over (mgmp_setup: a client's is READY, the host's
// is the chapter choice) and the closure is run later, by setup_tick, through a copy of the two fields it reads.
void equip_done_go(void* director) {
    alignas(16) uint8_t closure[0x20] = {};   // the original reads +0x8 (0: no chapter page) and +0x10 (the director), nothing else
    memcpy(closure + kEquipDone_Director, &director, sizeof(director));
    o_EquipDone(closure);
}

void __fastcall h_EquipDone(void* self) {
    uint8_t act_select = 1;
    void*   director   = nullptr;
    if (mem_read((const uint8_t*)self + kEquipDone_ActSelect, &act_select, sizeof(act_select)) && !act_select &&
        mem_read((const uint8_t*)self + kEquipDone_Director, &director, sizeof(director)) && director &&
        setup_on_equip_done(director, equip_done_go)) return;
    o_EquipDone(self);
}

// While a chapter map is generated its mapflag_* nodes answer with the flags EVERY player has (mgmp_unlocks). The getter destroys its key,
// so the original is always run -- for an overridden key only to let it do that -- and an inactive override costs one load and a branch.
int64_t __fastcall h_PropGetInt(void* props, void* key, int64_t fallback) {
    if (unlocks_override_active()) {
        char name[96];
        if (mem_read_std_string(key, name, sizeof(name))) {
            const int64_t local = o_PropGetInt(props, key, fallback);   // the getter destroys its key, so it always runs; its answer is the "this save says"
            int64_t value = 0;
            const int64_t answer = unlocks_override_lookup(name, local, value) ? value : local;
            if (lockstep_in_battle()) unlockq_prop(name, local, answer);
            return answer;
        }
    } else if (lockstep_in_battle()) {
        // a battle in a session, no override running (the host, or a client outside its window): the read is only RECORDED, so both peers' logs list what the battle asked of the save
        char name[96];
        if (mem_read_std_string(key, name, sizeof(name))) {
            const int64_t local = o_PropGetInt(props, key, fallback);
            unlockq_prop(name, local, local);
            return local;
        }
    }
    return o_PropGetInt(props, key, fallback);
}

// While a client builds a battle the host's unlock answers replace its own (mgmp_unlocks). The check destroys its name argument, so the original always
// runs -- its answer is the "this save says" half of the debug line.
bool unlock_detour(UnlockList list, fn_unlock_chk original, void* save, void* name) {
    if (!unlocks_window_active()) {
        if (!lockstep_in_battle()) return original(save, name);
        char text[96];                                  // a battle on the host (or a client outside its window): recorded, answered as the save does
        if (!mem_read_std_string(name, text, sizeof(text))) return original(save, name);
        const bool local = original(save, name);
        unlockq_query(list, text, local, local);
        return local;
    }
    char text[96];
    if (!mem_read_std_string(name, text, sizeof(text))) return original(save, name);
    const bool local = original(save, name);
    return unlocks_window_answer(list, text, local);
}
// The list of unlocked class names. The original runs; on a client in a battle's window the answer then becomes the host's list (mgmp_unlocks), and both peers record theirs.
void* __fastcall h_UnlockedClasses(void* save, void* out, bool with_colorless) {
    void* r = o_UnlockedClasses(save, out, with_colorless);
    unlocks_classes_after(out, with_colorless);
    return r;
}
bool __fastcall h_IsAbility(void* save, void* name) { return unlock_detour(UnlockList::Ability,   o_IsAbility, save, name); }
bool __fastcall h_IsPassive(void* save, void* name) { return unlock_detour(UnlockList::Passive,   o_IsPassive, save, name); }
bool __fastcall h_IsItem   (void* save, void* name) { return unlock_detour(UnlockList::Item,      o_IsItem,    save, name); }
bool __fastcall h_IsLevel  (void* save, void* name) { return unlock_detour(UnlockList::LevelGroup, o_IsLevel,   save, name); }
// select_boss_level's predicate: (closure, GonObject&) -- the boss's name is the std::string at +0x88 of the GonObject. It only reads it, so the original is
// asked first for the "this save says" half and the host's answer replaces it.
bool __fastcall h_IsBoss(void* closure, void* gon) {
    const bool local = o_IsBoss(closure, gon);
    if (!unlocks_window_active()) {
        if (lockstep_in_battle()) { char t[96]; if (mem_read_std_string((const uint8_t*)gon + 0x88, t, sizeof(t))) unlockq_query(UnlockList::Boss, t, local, local); }
        return local;
    }
    char text[96];
    if (!mem_read_std_string((const uint8_t*)gon + 0x88, text, sizeof(text))) return local;
    return unlocks_window_answer(UnlockList::Boss, text, local);
}
// The battle's party sort (0x364760, an insertion sort of the party's CatData* by speed): see mgmp_unlocks -- logged on both peers, and on a
// client put into the host's order when it came out differently.
// The level picker (0x394450): in a room every peer picks from the same shared stream, and the pick is logged (mgmp_unlocks). The fifth
// argument is a bool in a stack slot -- passed through whole.
typedef void* (__fastcall* fn_level_pick)(void* self, void* out, void* kind, void* node, uint64_t flag);
fn_level_pick o_LevelPick = nullptr;
void* __fastcall h_LevelPick(void* self, void* out, void* kind, void* node, uint64_t flag) {
    unlocks_level_pick(kind, node, nullptr, false);
    void* r = o_LevelPick(self, out, kind, node, flag);
    unlocks_level_pick(kind, node, r, true);
    return r;
}
// The stages of the battle build: each returns, then the shared stream is set from the battle id (mgmp_unlocks: unlocks_build_stage_end).
typedef void* (__fastcall* fn_stage)(void* self);
// An event outcome's `random_pool` (sub_1409310B0): both peers draw its entry from the same stream (mgmp_unlocks: unlocks_event_roll).
typedef void (__fastcall* fn_random_pool)(void* state, void* node);
fn_random_pool o_RandomPool = nullptr;
void __fastcall h_RandomPool(void* state, void* node) { unlocks_event_roll(); o_RandomPool(state, node); }
// Every other event keyword that draws from the shared stream (sub_1409173A0 calls each as handler(state, node [, cat])): the stream is reset from the node and the draw count first,
// so the host's click gives the same result on every peer (weather_roll = 'Happening', random_chance, reward, ...). Three register arguments are forwarded, the result is passed back.
typedef uint64_t (__fastcall* fn_ev_handler)(void* a, void* b, void* c);
fn_ev_handler o_EvWeatherRoll = nullptr;
uint64_t __fastcall h_EvWeatherRoll(void* a, void* b, void* c) { unlocks_event_roll(); return o_EvWeatherRoll(a, b, c); }
fn_ev_handler o_EvPoolLuck = nullptr;
uint64_t __fastcall h_EvPoolLuck(void* a, void* b, void* c) { unlocks_event_roll(); return o_EvPoolLuck(a, b, c); }
fn_ev_handler o_EvRandomChance = nullptr;
uint64_t __fastcall h_EvRandomChance(void* a, void* b, void* c) { unlocks_event_roll(); return o_EvRandomChance(a, b, c); }
fn_ev_handler o_EvReward = nullptr;
uint64_t __fastcall h_EvReward(void* a, void* b, void* c) { unlocks_event_roll(); return o_EvReward(a, b, c); }
fn_ev_handler o_EvDisorderPool = nullptr;
uint64_t __fastcall h_EvDisorderPool(void* a, void* b, void* c) { unlocks_event_roll(); return o_EvDisorderPool(a, b, c); }
fn_ev_handler o_EvLearnAbility = nullptr;
uint64_t __fastcall h_EvLearnAbility(void* a, void* b, void* c) { unlocks_event_roll(); return o_EvLearnAbility(a, b, c); }
fn_ev_handler o_EvLearnPassive = nullptr;
uint64_t __fastcall h_EvLearnPassive(void* a, void* b, void* c) { unlocks_event_roll(); return o_EvLearnPassive(a, b, c); }
fn_ev_handler o_EvMutSet = nullptr;
uint64_t __fastcall h_EvMutSet(void* a, void* b, void* c) { unlocks_event_roll(); return o_EvMutSet(a, b, c); }
fn_ev_handler o_EvMut = nullptr;
uint64_t __fastcall h_EvMut(void* a, void* b, void* c) { unlocks_event_roll(); return o_EvMut(a, b, c); }
// The option's stat check (sub_14091BE20, `stat int`/`str`/...: one draw against the chosen cat's stat gives good or bad, stored at event+0xF8). Without the reset the good/bad of
// DarkDen differed between the peers (host: CharmedBear familiar, client: MaddenedBear spawn) and the boards halted on a friendly bear (2026-10-03).
// The event's constructor: from here on its draws (difficulty, the subject cat) come from the node-seeded stream, the same on every peer.
fn_ev_handler o_EvSetup = nullptr;
uint64_t __fastcall h_EvSetup(void* a, void* b, void* c) { unlocks_event_roll(); return o_EvSetup(a, b, c); }
// "A random element of this vector" (sub_1400AB770, 41 callers). Only the call that picks an event's SUBJECT cat is taken over (the instruction after that call is kRet_EventSubject): the
// candidates are sorted by cat id and the draw indexes the sorted list, so both peers name the same cat even when their party lists are in another order (the client's own cats come first).
typedef void* (__fastcall* fn_pick_random)(void* vec, void* stream);
fn_pick_random o_PickRandom = nullptr;
void* __fastcall h_PickRandom(void* vec, void* stream) {
    static const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    if ((uintptr_t)_ReturnAddress() == base + kRet_EventSubject) {
        void* r = nullptr;
        if (unlocks_event_subject(vec, stream, (void*)o_PickRandom, &r)) return r;
    }
    return o_PickRandom(vec, stream);
}
// The save-properties int setter (increment/decrement_legacy_counter): inside an event window the new value is what the property reads as from then on (mgmp_unlocks: unlocks_property_set).
// The callee destroys the name string it is given, so it is read BEFORE the original runs.
typedef void (__fastcall* fn_prop_set_int)(void* name, int value);
fn_prop_set_int o_PropSetInt = nullptr;
// In a STORY event this peer only replays (the host decided it), the counters the result changes are not written to this peer's save: the reads inside the event still follow the new value (unlocks_property_set),
// which is what the host's own reads see. The callee destroys its name string; not calling it just leaves that string to be freed with the caller's frame.
void __fastcall h_PropSetInt(void* name, int value) {
    unlocks_property_set(name, value);
    if (choice_story_event_block_active()) { choice_story_event_blocked("a save property counter (increment/decrement_legacy_counter)"); return; }
    o_PropSetInt(name, value);
}

// THE EVENT RESULTS THAT CHANGE THE SAVE (2026-10-04). A story event's choice is the host's; the client replays it with the game's own commit, and the game runs the option's result script on the client's own save too:
// legacy tokens, quest progress, adventure unlocks. The client did not earn them (it carried no such item), so for the rest of such an event node each of these is skipped on the client. Everything that
// acts on the run itself (cats, items, coins, fights) still runs. None of the five has a return value the dispatcher (0x1409173A0) looks at.
typedef void (__fastcall* fn_res_ResSetLegacyToken)(void*, void*, void*, void*);
fn_res_ResSetLegacyToken o_ResSetLegacyToken = nullptr;
void __fastcall h_ResSetLegacyToken(void* a, void* b, void* c, void* d) {
    if (choice_story_event_block_active()) { choice_story_event_blocked("set_legacy_token"); return; }
    o_ResSetLegacyToken(a, b, c, d);
}
typedef void (__fastcall* fn_res_ResUnlockItemQuest)(void*, void*, void*, void*);
fn_res_ResUnlockItemQuest o_ResUnlockItemQuest = nullptr;
void __fastcall h_ResUnlockItemQuest(void* a, void* b, void* c, void* d) {
    if (choice_story_event_block_active()) { choice_story_event_blocked("unlock_item_quest"); return; }
    o_ResUnlockItemQuest(a, b, c, d);
}
typedef void (__fastcall* fn_res_ResAdventureUnlock)(void*, void*, void*, void*);
fn_res_ResAdventureUnlock o_ResAdventureUnlock = nullptr;
void __fastcall h_ResAdventureUnlock(void* a, void* b, void* c, void* d) {
    if (choice_story_event_block_active()) { choice_story_event_blocked("trigger_adventure_unlock"); return; }
    o_ResAdventureUnlock(a, b, c, d);
}
typedef void (__fastcall* fn_res_ResCompleteItemQuest)(void*, void*, void*, void*);
fn_res_ResCompleteItemQuest o_ResCompleteItemQuest = nullptr;
void __fastcall h_ResCompleteItemQuest(void* a, void* b, void* c, void* d) {
    if (choice_story_event_block_active()) { choice_story_event_blocked("complete_item_quest"); return; }
    o_ResCompleteItemQuest(a, b, c, d);
}
// The item an event result gives (get_item, get_item_from_pool, get_and_equip_item, ...): the pool PICK before it has already drawn from the shared stream on both peers (mgmp_hooks h_EvItemPick), so skipping the give
// here keeps the streams together. On the client of a story event the item would land in its own inventory (or on the host's cat's copy, which the host's push replaces anyway) and is a story item it did not earn.
typedef void (__fastcall* fn_res_ResGiveItem)(void*, void*, void*, void*);
fn_res_ResGiveItem o_ResGiveItem = nullptr;
void __fastcall h_ResGiveItem(void* a, void* b, void* c, void* d) {
    if (choice_story_event_block_active()) { choice_story_event_blocked("the item a story event gives (it is not put in this peer's inventory or on a cat here)"); return; }
    o_ResGiveItem(a, b, c, d);
}
typedef void (__fastcall* fn_res_ResDejaVu)(void*, void*, void*, void*);
fn_res_ResDejaVu o_ResDejaVu = nullptr;
void __fastcall h_ResDejaVu(void* a, void* b, void* c, void* d) {
    if (choice_story_event_block_active()) { choice_story_event_blocked("increment_deja_vu"); return; }
    o_ResDejaVu(a, b, c, d);
}

// The House state writer: before it writes `files.house_state`, the House cat entities that still carry a clone's id get the id of the cat that clone was swapped into (mgmp_catsync).
// Without it the file named the clone ids, and the next load brought the clones back to life (flags 1, not retired) while the retired originals were not loaded at all.
fn_ev_handler o_HouseSave = nullptr;
uint64_t __fastcall h_HouseSave(void* a, void* b, void* c) { catsync_house_save(a); return o_HouseSave(a, b, c); }
// The pool item picker behind get_item_from_pool (and its siblings): it draws from the shared stream, but what the event did just before (party_damage hits each peer's OWN party, whose
// on-damage passives draw too) is not the same on every peer -- Baphomet gave one peer the Cancer trinket (2026-10-03). Called from the WorldEvent code only: the stream is reset first.
typedef void* (__fastcall* fn_item_pick)(void* a, void* b, void* c, double luck);
fn_item_pick o_EvItemPick = nullptr;
void* __fastcall h_EvItemPick(void* a, void* b, void* c, double luck) {
    static const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    const uintptr_t ra = (uintptr_t)_ReturnAddress() - base;
    if ((ra >= 0x9173A0 && ra < 0x919752) || (ra >= 0x91F5F0 && ra < 0x9205F7)) unlocks_event_roll();
    return o_EvItemPick(a, b, c, luck);
}
fn_ev_handler o_EvStatCheck = nullptr;
uint64_t __fastcall h_EvStatCheck(void* a, void* b, void* c) { unlocks_event_roll(); return o_EvStatCheck(a, b, c); }
fn_stage o_BuildStatics = nullptr;
void* __fastcall h_BuildStatics(void* self) { void* r = o_BuildStatics(self); unlocks_build_stage_end("statics"); return r; }
fn_stage o_BuildTerrain = nullptr;
void* __fastcall h_BuildTerrain(void* self) { void* r = o_BuildTerrain(self); unlocks_build_stage_end("terrain"); return r; }
fn_stage o_BuildProps = nullptr;
void* __fastcall h_BuildProps(void* self) { void* r = o_BuildProps(self); unlocks_build_stage_end("props"); return r; }
fn_stage o_BuildParty = nullptr;
void* __fastcall h_BuildParty(void* self) { void* r = o_BuildParty(self); unlocks_build_stage_end("party"); return r; }
fn_stage o_BuildAllies = nullptr;
void* __fastcall h_BuildAllies(void* self) { void* r = o_BuildAllies(self); unlocks_build_stage_end("allies"); return r; }
fn_stage o_BuildExtra = nullptr;
void* __fastcall h_BuildExtra(void* self) { void* r = o_BuildExtra(self); unlocks_build_stage_end("extra"); return r; }
fn_stage o_BuildEnemies = nullptr;
void* __fastcall h_BuildEnemies(void* self) { void* r = o_BuildEnemies(self); unlocks_build_stage_end("enemies"); return r; }
// The battle build (0x35AAF0): in a room every peer starts it from the same shared stream (mgmp_unlocks).
typedef void* (__fastcall* fn_battle_build)(void* a, void* b, void* c, void* d);
fn_battle_build o_BattleBuild = nullptr;
void* __fastcall h_BattleBuild(void* a, void* b, void* c, void* d) {
    unlocks_battle_build();
    void* r = o_BattleBuild(a, b, c, d);
    unlocks_battle_build_done();
    return r;
}
typedef void** (__fastcall* fn_spawn_sort)(void** first, void** last);
fn_spawn_sort o_SpawnSort = nullptr;
void** __fastcall h_SpawnSort(void** first, void** last) {
    unlocks_spawn_sort(first, last, false);
    void** r = o_SpawnSort(first, last);
    unlocks_spawn_sort(first, last, true);
    return r;
}

void __fastcall h_GenerateMap(void* self) {
    setup_on_generate_map();
    o_GenerateMap(self);
}

void __fastcall h_SelectAct(void* self, int act) {
    if (!setup_on_select_act(act)) return;
    o_SelectAct(self, act);
}

void __fastcall h_ButtonUpdate(void* self) {
    // Fires for every button in the game. Outside a combat-menu tick this is a
    // load and a branch: combatlock_on_button's first test is the scope flag.
    combatlock_on_button(self);

    // Same audience, a different question: this one names the screens themselves,
    // which is what locating the run's setup flow needs. Reads only, gated on
    // tune::kLogButtons, and it returns on the first byte of a 256-entry table
    // once a name has been seen. See mgmp_listprobe.h.
    listprobe_on_button(self);
    menu_on_button(self);

    // Which named buttons are up is what identifies the screens between the House
    // and the chapter page -- they are panels, not scenes. See mgmp_page.h.
    page_on_button(self);

    // Route A (2026-09-23): the same per-button detour is what recognises the gear
    // screen and re-aims the one pointer it reads its cat list through. On every frame,
    // for every button, and it does nothing at all unless that screen is up.
    roster_catselect_redirect(self);

    // Optional independent cross-check of the upgrade screen's subject pointer.
    choice_on_button(self);

    // And the one button question that is not instrumentation: is the CHAPTER
    // PAGE up. Its two difficulty buttons ticking is what the run's barrier will
    // gate on, and this is the only place they can be seen. See mgmp_leave.h.
    leave_on_button(self);
    chapter_control(self);

    o_ButtonUpdate(self);
    chapter_control(self);

    // AFTER the original, and this one has to be. Button::update recomputes the
    // button's state and pushes it into the SWF before it returns, and
    // Button::Click's own guards read that state -- clicking first would test
    // the previous frame's. It is also where the game's own click would have
    // landed: update dispatches at the bottom of its hover branch.
    //
    // Same trampoline reasoning as everywhere else: Button::Click is a call
    // target, not a hook, so nothing here re-enters this detour.
    savefile_on_button_update(self);

    // The other half of the same idea: Play on the main menu gets a client
    // INTO the host's run, Quit To Menu in the pause sidebar gets it back
    // OUT when the host has left. Same slot, same trampoline reasoning.
    leave_on_button_update(self);
}

// The aim preview turns the acting cat while a decision is held, writing the
// same Character+0x388 the backstab test reads -- under a wall-clock gate, so
// two peers at different frame rates disagree about damage. Snapshot the field,
// let the preview do whatever it likes, put it back. Inert outside a session.
//
// `out` is the caller's TurnAction sret buffer. It is not read or written here
// -- it exists in the signature so that rdx and the returned rax survive the
// detour untouched.
void* __fastcall h_UpdateDecision(void* self, void* out) {
    lockstep_preview_facing_begin(self);
    void* ret = o_UpdateDecision(self, out);
    lockstep_preview_facing_end();

    // AFTER the original, deliberately. The game's own aim preview draws from
    // inside it, so drawing the peer's from here puts the two on the same
    // frame, in the same order, at the same layer -- and on the peer that does
    // NOT own this cat the original drew nothing anyway, because GetChoice was
    // overwritten with "nothing decided" and the type-2 guard never passed.
    aim_on_update_decision(self);
    return ret;
}


// --- phase 5: the client never picks a save file ---------------------------

// See kSaveSel_Picked: a pick the mod holds must not leave the save screen frozen.
static void savesel_set_picked(void* ss, bool on) {
    if (!ss) return;
    const uint8_t v = on ? 1 : 0;
    mem_write((uint8_t*)ss + kSaveSel_Picked, &v, 1);
}

void __fastcall h_SaveSlotClick(void* ss, int slot, unsigned char play_sound) {
    // Same shape as h_EnterNode, and for the same reason: the client's own pick
    // is swallowed here, and the auto-select below calls o_SaveSlotClick -- the
    // MinHook trampoline -- which bypasses this detour outright, so no
    // "am I injecting" flag is needed.
    if (!savefile_on_slot_click(ss, slot)) {
        // The game marked the screen "picked" before it got here; the pick is the mod's to hold now,
        // not the screen's. Clearing the flag gives back hover, the other pods and the way out.
        savesel_set_picked(ss, false);
        return;
    }
    savesel_set_picked(ss, true);
    choice_reset_run();
    o_SaveSlotClick(ss, slot, play_sound);
}

void __fastcall h_SaveSelUpdate(void* self) {
    o_SaveSelUpdate(self);
    { const int want = menu_take_slot_request(); if (want >= 0) { h_SaveSlotClick(self, want, 0); return; } }

    // After the original: the native click arrives from a Button callback fired
    // inside this same update, so entering the slot once it has returned is the
    // same point in the frame rather than a reentrant call into a half-updated
    // screen.
    if (!o_SaveSlotClick) return;
    int slot = savefile_autoselect(self);
    if (slot >= 0) { savesel_set_picked(self, true); o_SaveSlotClick(self, slot, 0); }
}

// The client loads the host's run from its own file rather than from the slot
// the host was playing, so the player's real saves survive a co-op session.
// MewDirector::init takes its filename BY VALUE -- a hidden pointer the callee
// reads and does not own -- so handing it a std::string of ours is exactly as
// valid as the vector element the game would have passed.
void* __fastcall h_MewDirInit(void* self, void* name) {
    if (const void* sub = savefile_redirect_load())
        return o_MewDirInit(self, (void*)sub);
    return o_MewDirInit(self, name);
}

// The two inventory blob accessors. Both bodies are one predicted branch on a
// flag that only mgmp_invsync's own calls ever set -- every OTHER blob in the
// save file passes through here too, and rewriting those would be rewriting the
// save. When the intercept returns true it has already destroyed the key
// string, because the function we are standing in for would have.
//
// Returning null is safe: sub_14022C4D0 and sub_14022C620 both tail into
// std::string::_Tidy_deallocate, whose return value is junk, and no call site
// of either reads it.
void* __fastcall h_SFStoreBlob(void* self, void* key, void* bs) {
    if (invsync_intercept_store(key, bs)) return nullptr;
    return o_SFStoreBlob(self, key, bs);
}

void* __fastcall h_SFLoadBlob(void* self, void* key, void* bs) {
    if (invsync_intercept_load(key, bs)) return nullptr;
    return o_SFLoadBlob(self, key, bs);
}

void __fastcall h_FrameBegin(void* self) {
    static LONG volatile frames = 0;
    LONG n = InterlockedIncrement(&frames);
    uint32_t every = tune::kFrameLogEvery;
    if (every && (n == 1 || (n % (LONG)every) == 0))
        log_line("FRAME", "frame %ld", n);

    // The RE watchpoint tick. Nothing but four guarded reads a frame when no
    // battle has been seen yet, and it is the only place the chain can be
    // re-walked often enough to catch the battle replacing its cat vector.
    listprobe_tick();

    // Where this peer is, for the room panel. Polls on its own divisor (250 ms) and
    // only sends when a session exists -- but it reads on every frame, because a
    // player can be in a room long before a socket is up. See mgmp_page.h.
    page_tick();
    catview_tick();
    abandon_tick();
    room_tick();
    checkpoint_tick();
    setup_tick();
    unlocks_tick();


    // Frame markers are what let Run D ask its question: with identical actions
    // and a deliberately stalled frame rate, does the draw sequence between two
    // actions change? Off by default because on a clean recording they are just
    // volume.
    if (tune::kRecordFrames) record_frame((uint32_t)n);

    // The network is pumped here rather than at a turn boundary so messages
    // keep arriving while a brain is polled -- a peer's decision has to be able
    // to land in the middle of our wait for it, not only between turns.
    session_update();

    o_FrameBegin(self);
}

// ---- installation --------------------------------------------------------

struct Binding {
    Target target;
    void*  detour;
    void** original;
};

const Binding kBindings[] = {
    { T_InitSystems, (void*)&h_InitSystems, (void**)&o_InitSystems },
    { T_NextTurn,    (void*)&h_NextTurn,    (void**)&o_NextTurn    },
    { T_GetChoice,   (void*)&h_GetChoice,   (void**)&o_GetChoice   },
    { T_DoAction,    (void*)&h_DoAction,    (void**)&o_DoAction    },
    { T_Trigger,     (void*)&h_Trigger,     (void**)&o_Trigger     },
    { T_BeginTurn,   (void*)&h_BeginTurn,   (void**)&o_BeginTurn   },
    { T_EndTurn,     (void*)&h_EndTurn,     (void**)&o_EndTurn     },
    { T_FrameBegin,  (void*)&h_FrameBegin,  (void**)&o_FrameBegin  },
    { T_ApplyAction, (void*)&h_ApplyAction, (void**)&o_ApplyAction },

    { T_QueueDecision, (void*)&h_QueueDecision, (void**)&o_QueueDecision },

    { T_MapUpdate,   (void*)&h_MapUpdate,   (void**)&o_MapUpdate   },
    { T_EnterNode,   (void*)&h_EnterNode,   (void**)&o_EnterNode   },

    { T_StatusMenuUpdate, (void*)&h_StatusMenu, (void**)&o_StatusMenu },

    { T_EventChoice,  (void*)&h_EventChoice, (void**)&o_EventChoice },
    { T_EventUpdate,  (void*)&h_EventUpdate, (void**)&o_EventUpdate },
    { T_LevelSelect,  (void*)&h_LevelSelect, (void**)&o_LevelSelect },
    { T_LevelUpdate,  (void*)&h_LevelUpdate, (void**)&o_LevelUpdate },

    { T_SaveSelUpdate,  (void*)&h_SaveSelUpdate,  (void**)&o_SaveSelUpdate  },
    { T_SaveSlotClick,  (void*)&h_SaveSlotClick,  (void**)&o_SaveSlotClick  },
    { T_MewDirectorInit,(void*)&h_MewDirInit,     (void**)&o_MewDirInit     },

    { T_SFStoreBlob,    (void*)&h_SFStoreBlob, (void**)&o_SFStoreBlob },
    { T_SFLoadBlob,     (void*)&h_SFLoadBlob,  (void**)&o_SFLoadBlob  },

    { T_CombatMenuUpdate, (void*)&h_CombatMenu,   (void**)&o_CombatMenu   },
    { T_ButtonUpdate,     (void*)&h_ButtonUpdate, (void**)&o_ButtonUpdate },
    { T_EquipmentClick, (void*)&h_EquipmentClick, (void**)&o_EquipmentClick },
    { T_EndRunFinalize, (void*)&h_EndRunFinalize, (void**)&o_EndRunFinalize },
    { T_EndRunDefeat,   (void*)&h_EndRunDefeat,   (void**)&o_EndRunDefeat },
    { T_GainCat,        (void*)&h_GainCat,        (void**)&o_GainCat },
    { T_EquipDone,      (void*)&h_EquipDone,      (void**)&o_EquipDone },
    { T_PropGetInt,     (void*)&h_PropGetInt,     (void**)&o_PropGetInt },
    { T_GenerateMap,    (void*)&h_GenerateMap,    (void**)&o_GenerateMap },
    { T_IsAbilityUnlocked, (void*)&h_IsAbility,   (void**)&o_IsAbility },
    { T_IsPassiveUnlocked, (void*)&h_IsPassive,   (void**)&o_IsPassive },
    { T_IsItemUnlocked,    (void*)&h_IsItem,      (void**)&o_IsItem },
    { T_IsLevelUnlocked,   (void*)&h_IsLevel,     (void**)&o_IsLevel },
    { T_IsBossAvailable,   (void*)&h_IsBoss,      (void**)&o_IsBoss },
    { T_SpawnSort,         (void*)&h_SpawnSort,   (void**)&o_SpawnSort },
    { T_BattleBuild,       (void*)&h_BattleBuild, (void**)&o_BattleBuild },
    { T_LevelPick,         (void*)&h_LevelPick,   (void**)&o_LevelPick },
    { T_BuildStatics,    (void*)&h_BuildStatics,  (void**)&o_BuildStatics },
    { T_BuildTerrain,    (void*)&h_BuildTerrain,  (void**)&o_BuildTerrain },
    { T_BuildProps,      (void*)&h_BuildProps,    (void**)&o_BuildProps },
    { T_BuildParty,      (void*)&h_BuildParty,    (void**)&o_BuildParty },
    { T_BuildAllies,     (void*)&h_BuildAllies,   (void**)&o_BuildAllies },
    { T_BuildExtra,      (void*)&h_BuildExtra,    (void**)&o_BuildExtra },
    { T_BuildEnemies,    (void*)&h_BuildEnemies,  (void**)&o_BuildEnemies },
    { T_RandomPool,      (void*)&h_RandomPool,    (void**)&o_RandomPool },
    { T_EvWeatherRoll, (void*)&h_EvWeatherRoll, (void**)&o_EvWeatherRoll },
    { T_EvPoolLuck, (void*)&h_EvPoolLuck, (void**)&o_EvPoolLuck },
    { T_EvRandomChance, (void*)&h_EvRandomChance, (void**)&o_EvRandomChance },
    { T_EvReward, (void*)&h_EvReward, (void**)&o_EvReward },
    { T_EvDisorderPool, (void*)&h_EvDisorderPool, (void**)&o_EvDisorderPool },
    { T_EvLearnAbility, (void*)&h_EvLearnAbility, (void**)&o_EvLearnAbility },
    { T_EvLearnPassive, (void*)&h_EvLearnPassive, (void**)&o_EvLearnPassive },
    { T_EvMutSet, (void*)&h_EvMutSet, (void**)&o_EvMutSet },
    { T_EvMut, (void*)&h_EvMut, (void**)&o_EvMut },
    { T_EvStatCheck, (void*)&h_EvStatCheck, (void**)&o_EvStatCheck },
    { T_EvSetup, (void*)&h_EvSetup, (void**)&o_EvSetup },
    { T_PickRandom, (void*)&h_PickRandom, (void**)&o_PickRandom },
    { T_PropSetInt, (void*)&h_PropSetInt, (void**)&o_PropSetInt },
    { T_ResSetLegacyToken, (void*)&h_ResSetLegacyToken, (void**)&o_ResSetLegacyToken },
    { T_ResUnlockItemQuest, (void*)&h_ResUnlockItemQuest, (void**)&o_ResUnlockItemQuest },
    { T_ResAdventureUnlock, (void*)&h_ResAdventureUnlock, (void**)&o_ResAdventureUnlock },
    { T_ResCompleteItemQuest, (void*)&h_ResCompleteItemQuest, (void**)&o_ResCompleteItemQuest },
    { T_ResGiveItem, (void*)&h_ResGiveItem, (void**)&o_ResGiveItem },
    { T_ResDejaVu, (void*)&h_ResDejaVu, (void**)&o_ResDejaVu },
    { T_HouseSave, (void*)&h_HouseSave, (void**)&o_HouseSave },
    { T_EvItemPick, (void*)&h_EvItemPick, (void**)&o_EvItemPick },
    { T_UnlockedClasses, (void*)&h_UnlockedClasses, (void**)&o_UnlockedClasses },
    { T_TryAbandon,     (void*)&h_TryAbandon,     (void**)&o_TryAbandon     },
    { T_SelectAct,        (void*)&h_SelectAct, (void**)&o_SelectAct },
    { T_ChapterLower,     (void*)&h_ChapterLower, (void**)&o_ChapterLower },
    { T_ChapterRaise,     (void*)&h_ChapterRaise, (void**)&o_ChapterRaise },
    { T_HighlightRefresh, (void*)&h_HighlightRefresh, (void**)&o_HighlightRefresh },
    { T_UpdateDecision,   (void*)&h_UpdateDecision, (void**)&o_UpdateDecision },

    { T_SaveScumPenalty, (void*)&h_SaveScum, (void**)&o_SaveScum },
    { T_SaveScumWarn,    (void*)&h_ScumWarn, (void**)&o_ScumWarn },
    { T_TryDepart,       (void*)&h_TryDepart, (void**)&o_TryDepart },
    { T_IsKitten,        (void*)&h_IsKitten,  (void**)&o_IsKitten },
    { T_LoadChar,        (void*)&h_LoadChar,  (void**)&o_LoadChar },
    { T_TimeDelayTick,   (void*)&h_TimeDelay, (void**)&o_TimeDelay },

    // Detours and originals live in mgmp_rng.cpp so the hot path is compiled
    // without the tracing machinery this file pulls in.
    { T_RandInt,   nullptr, nullptr },
    { T_RandFloat, nullptr, nullptr },
    { T_Rand2,     nullptr, nullptr },
    { T_RollChance,nullptr, nullptr },
};

// Resolves the three RNG bindings, which are not static initialisers because
// their detours are defined in another translation unit.
bool rng_binding(Target t, void** detour, void*** original) {
    switch (t) {
    case T_RandInt:   *detour = rng_detour_randint();   *original = rng_original_randint();   return true;
    case T_RandFloat: *detour = rng_detour_randfloat(); *original = rng_original_randfloat(); return true;
    case T_Rand2:     *detour = rng_detour_rand2();     *original = rng_original_rand2();     return true;
    case T_RollChance:*detour = rng_detour_rollchance();*original = rng_original_rollchance();return true;
    default: return false;
    }
}

// Install one binding. Returns true only if it went live on THIS call, so a
// caller can count what it actually added.
//
// `announce_off` separates the two passes. The startup pass lists every hook it
// did not install, because that banner is meant to be a complete inventory of
// what this process is running. The late pass (hooks_install_late) is only
// interested in what it managed to add -- repeating the "off by default" lines
// there would print the same inventory twice.
bool install_one(const Binding& b, bool announce_off) {
    const TargetDesc& t = kTargets[b.target];

    // Already live. The late pass walks the whole table, so this is the normal
    // case there rather than an error.
    if (b.target >= 0 && b.target < T_COUNT && g_live[b.target]) return false;

    if (!config().hook[b.target]) {
        if (announce_off) {
            // The RNG hooks default off and are implied by debug.record, so
            // a flat "disabled" was misleading for them -- it read as if
            // something had been turned off when it had not.
            bool is_rng = (b.target == T_RandInt || b.target == T_RandFloat ||
                           b.target == T_Rand2);
            log_raw("  [-] %-9s %s (%s)", t.name, t.symbol,
                    is_rng && !config().record ? "off: needs debug.record"
                                               : "off by default");
        }
        return false;
    }

    void*  detour   = b.detour;
    void** original = b.original;
    rng_binding(b.target, &detour, &original);
    if (!detour || !original) {
        log_raw("  [!] %-9s no detour bound", t.name);
        return false;
    }

    // Resolved by signature, not by RVA. Zero means it did not resolve at
    // all, and resolve_init has already said so and decided whether that
    // was survivable -- here it is simply a hook we do not install.
    void* addr = (void*)addr_of(b.target);
    if (!addr) {
        log_raw("  [!] %-9s unresolved -- not hooking", t.name);
        return false;
    }
    MH_STATUS s = MH_CreateHook(addr, detour, original);
    if (s != MH_OK) {
        log_raw("  [!] %-9s MH_CreateHook failed (%d) at %p", t.name, (int)s, addr);
        return false;
    }
    s = MH_EnableHook(addr);
    if (s != MH_OK) {
        log_raw("  [!] %-9s MH_EnableHook failed (%d) at %p", t.name, (int)s, addr);
        return false;
    }
    log_raw("  [+] %-9s %p  %s", t.name, addr, t.symbol);
    if (b.target >= 0 && b.target < T_COUNT) g_live[b.target] = true;
    return true;
}

} // namespace

bool hooks_verify_module(uintptr_t base, char* err, size_t err_size) {
    g_base = base;

    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    uint32_t size_of_image = nt->OptionalHeader.SizeOfImage;

    // A DIFFERENT BUILD IS NO LONGER A REASON TO REFUSE.
    //
    // This used to return false here, which meant that on the day the game
    // patched, the mod would not even attempt to load -- the signature
    // machinery below would never get to run, and the one mechanism built to
    // survive an update would be skipped by the check guarding it. The size is
    // still worth stating, because it tells the reader of a log which build
    // produced it, but it decides nothing.
    if (size_of_image != kExpectedSizeOfImage) {
        log_raw("[~] SizeOfImage 0x%X != the pinned 0x%X -- this is NOT the build "
                "the signatures were generated from. Resolving by signature.",
                size_of_image, kExpectedSizeOfImage);
    }
    // ...BUT THE STRUCTURE LAYOUTS ARE NOT SIGNATURE-CHECKED (2026-10-01, before the first release). Finding a function
    // after a patch says nothing about the offsets the mod then reads and writes in the objects it is handed; on a
    // patched game that is silent memory corruption. So an unpinned build is refused (the game runs unmodded) unless
    // the player says otherwise in mgmp.json.
    if ((nt->FileHeader.TimeDateStamp != kExpectedTimeStamp || size_of_image != kExpectedSizeOfImage) &&
        !config().allow_unpinned) {
        _snprintf_s(err, err_size, _TRUNCATE,
                    "this Mewgenics.exe (timestamp %08X, size %X) is not the build the mod was made for (%08X, %X); "
                    "set debug.allow_unpinned_build = true in mgmp.json to try anyway",
                    nt->FileHeader.TimeDateStamp, size_of_image, kExpectedTimeStamp, kExpectedSizeOfImage);
        return false;
    }

    // Everything now hangs off this: it resolves all 49 addresses, reports any
    // that moved, and returns false only when something lockstep cannot be
    // correct without has gone missing entirely.
    if (!resolve_init(base)) {
        _snprintf_s(err, err_size, _TRUNCATE,
                    "a critical hook target could not be resolved by signature");
        return false;
    }
    return true;
}

int hooks_install() {
    if (MH_Initialize() != MH_OK) {
        log_raw("  [!] MH_Initialize failed");
        return -1;
    }

    rng_set_base(g_base);
    catsync_set_base(g_base);
    invsync_set_base(g_base);
    runhist_set_base(g_base);
    nodehash_set_base(g_base);
    aim_set_base(g_base);
    lockstep_set_base(g_base);
    cursor_set_base(g_base);
    choice_set_base(g_base);
    savefile_set_base(g_base);
    leave_set_base(g_base);
    overlay_set_base(g_base);

    int installed = 0;
    for (const Binding& b : kBindings)
        if (install_one(b, /*announce_off=*/true)) ++installed;

    // After every hook is known, not before: a feature that is only safe while
    // another hook runs has to be told, and aim_set_base ran above -- before
    // anything was installed -- so it could not have asked then.
    aim_on_hooks_installed();

    // Last, and after MinHook is up. The roster probe installs no hook of its
    // own -- it watches memory -- so it only needs the process to be past the
    // point where a vectored handler can still be added safely.
    listprobe_init();

    // Same shape, and it hooks nothing either: it verifies one function's
    // prologue and refuses to call it if that fails. Doing it here rather than at
    // first use means a drifted address is one error line at startup instead of a
    // refusal in the middle of a run edit.
    roster_init();
    return installed;
}

int hooks_install_late() {
    int installed = 0;
    for (const Binding& b : kBindings)
        if (install_one(b, /*announce_off=*/false)) ++installed;

    // Re-asked for the same reason it is asked at startup, and it MATTERS here:
    // T_HighlightRefresh is one of the hooks this pass exists to add, and
    // mgmp_aim refuses to call the ability highlight without it. Skipping this
    // would leave the aim preview permanently disarmed in exactly the session
    // that just fixed its hooks.
    if (installed) aim_on_hooks_installed();
    return installed;
}

bool hooks_is_live(int target) {
    return target >= 0 && target < T_COUNT && g_live[target];
}

void hooks_uninstall() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

} // namespace mgmp
