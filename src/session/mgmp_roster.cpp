// mgmp_roster.cpp -- see the header: what it writes, why that is the primitive
// the two-peer party needs, and why it refuses loudly instead of guessing.
//
// The one rule this file follows is the opposite of the watchpoint instrument's:
// THAT one observes and never writes, THIS one writes and never speculates. Every
// value it replaces and every value it leaves behind is in the log, because a
// run edit that cannot be read back out of a log is indistinguishable from a
// desync -- and this project has already spent a night on that mistake.

#include "mgmp_roster.h"

#include "mgmp_config.h"
#include "mgmp_tuning.h"      // kCatSelectRedirect -- route A's switch (2026-09-23)
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_addresses.h"   // kRva_MewDirectorPtr, kDir_CatIdCount/Data
#include "mgmp_resolve.h"     // addr_of_call -- C_CatIdAppend, the game's own vector
                              // append, which is how a party GROWS safely (2026-09-23)
#include "mgmp_net.h"         // net_role, net_active, net_send_party
#include "mgmp_proto.h"       // PartyMsg, kMaxPartyIds
#include "mgmp_lockstep.h"    // lockstep_cat_is_mine -- WHO a cat belongs to, the one
                              // reader that works on the map (see roster_party_tick,
                              // 4+4 steps 1-2: each peer publishes only its own half)

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// The chain, the same offsets mgmp_lockstep and mgmp_listprobe walk: the scene is
// at TurnControl+0x18, and the object carrying both tables at Scene+0x08. Copied
// rather than shared for the same reason those two do it -- each file means a
// different thing by the pointer it ends up with.
constexpr uintptr_t kTC_Scene   = 0x18;
constexpr uintptr_t kScene_Sub  = 0x08;
constexpr uintptr_t kSub_Live   = 0x20;   // 16-byte entries: {container*, valid}
constexpr uintptr_t kSub_Source = 0x18;   // 16-byte entries: {cap, count, array}
constexpr uintptr_t kSlot       = 0x1F90; // entry 505 in both tables, 0x1F90/16
constexpr int32_t   kSlotIndex  = 505;

constexpr uintptr_t kSrc_Cap   = 0;       // offsets inside a SOURCE entry
constexpr uintptr_t kSrc_Count = 4;
constexpr uintptr_t kSrc_Array = 8;

constexpr uintptr_t kCtr_Count = 0x0C;    // inside the live CONTAINER: {refcount, pad, cap, count, data}

// void sub_14096B470(sub, int index). The prologue is pinned from a live read of
// the running process and checked before the first call, because this is a CALL
// SITE: a watchpoint on a wrong address reports somebody else's traffic, while a
// call to a wrong address is a crash.
//
// THE `0x40` ON THE FIRST BYTE IS THE WHOLE REASON THIS IS A LIVE READ. The
// disassembly renders it as `push rsi` with the REX prefix folded into the
// instruction, and the first version of this array was transcribed from that
// listing one byte short -- whereupon the check refused, correctly, and the
// refusal was the only reason a crash did not happen later. Read from the
// process (2026-09-22, base+0x96B470):
//     40 56 41 56 41 57 48 83 ec 40 48 8b 41 20 48 8b ...
// The same read confirmed base+rva is the right way to reach it: base+0x96B52B
// on the same process is `4c 89 64 24 38 4c 8b e5`, byte for byte what the
// watchpoint probe has pinned for the append chunk since 2026-09-21.
constexpr uint32_t kRva_Resync = 0x96B470;
constexpr uint8_t  kResyncProlog[17] = {
    0x40,                    // REX prefix -- part of the instruction, easy to lose
    0x56,                    // push rsi
    0x41, 0x56,              // push r14
    0x41, 0x57,              // push r15
    0x48, 0x83, 0xEC, 0x40,  // sub  rsp, 0x40
    0x48, 0x8B, 0x41, 0x20,  // mov  rax, [rcx+0x20]   (the live table)
    0x48, 0x8B, 0xF1,        // mov  rsi, rcx          (keep `sub`)
};

struct State {
    bool     on      = false;
    bool     tried   = false;
    void*    tc      = nullptr;
    void   (*resync)(void*, int) = nullptr;

    // --- the party exchange ---
    uint32_t my_ids[kMaxPartyIds]   = {};   // the run's list as we last read it
    uint32_t my_n                   = 0;
    uint32_t peer_ids[kMaxPartyIds] = {};   // the last picks a peer sent
    uint32_t peer_n                 = 0;
    uint32_t peers_seen             = 0;    // how many peers have sent picks
    uint32_t agreed[kMaxPartyIds]   = {};   // what peer 0 decided
    uint32_t agreed_n               = 0;
    bool     said_agree             = false;

    bool     applied = false;   // the debug number has been used; one edit per process
    bool     said_no_tc  = false;   // wait states are explained once, not silently
    bool     said_no_sub = false;
    bool     said_from_scenes = false;   // the no-battle route found it
    uint32_t ticks = 0;             // how many map ticks have been offered
    uint32_t last_src_before = 0, last_src_after = 0;
    uint32_t last_live_before = 0, last_live_after = 0;
    bool     last_ok = false;
    uint64_t turn_serial = 0;
};
State g;

// THE PATH THAT WAS TRIED AND DISPROVEN, kept as a note rather than as code.
//
// The first version of this module edited slot 505 of the `sub` object reached
// through the TurnControl, and -- to avoid needing a battle at all -- tried to
// find that object from the loaded-scene list (`Director+0/+8`, `Scene+8`). Both
// halves were wrong, and the live process said so on 2026-09-22: `Scene+8` is not
// that object (the candidates read as `0x290000002A`, not pointers), and slot 505
// itself is BATTLE-SCOPED -- released when the fight ends, which is why the edit
// refused with "did not read as {cap, count, array} plus a live container".
//
// What is real is MewDirector+1468/+1472, the run's cat id vector -- see
// roster_tick. Do not resurrect this: the scene walk belongs to mgmp_leave, which
// owns "which screen is up", and it is not a route to the run object.

} // namespace

void roster_init() {
    if (g.tried) return;
    g.tried = true;

    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    if (!base) return;
    const uintptr_t fn = base + kRva_Resync;

    uint8_t got[sizeof(kResyncProlog)] = {};
    if (!mem_read((const void*)fn, got, sizeof(got)) ||
        memcmp(got, kResyncProlog, sizeof(got)) != 0) {
        // THE BYTES GO IN THE LINE, both of them. The first version of this check
        // refused and said only that it had -- which left "the address moved" and
        // "the pinning is wrong" indistinguishable without attaching to a live
        // process a second time. It was the pinning (a REX prefix lost in
        // transcription), and one hex line would have said so immediately.
        char want[64] = {}, have[64] = {};
        for (size_t i = 0; i < sizeof(kResyncProlog); ++i) {
            _snprintf_s(want + i * 3, sizeof(want) - i * 3, _TRUNCATE, "%02X ",
                        kResyncProlog[i]);
            _snprintf_s(have + i * 3, sizeof(have) - i * 3, _TRUNCATE, "%02X ",
                        got[i]);
        }
        log_line_lvl(LogLevel::Error, "ROSTER",
                     "!! rva 0x%X does not start with the resync function's prologue --"
                     " the roster editor is OFF. A call to a wrong address is a crash,"
                     " where a watchpoint on one is only a bad reading, so this refuses"
                     " rather than tries.\n     expected: %s\n     found   : %s",
                     (unsigned)kRva_Resync, want, have);
        return;
    }

    g.resync = (void (*)(void*, int))fn;
    g.on     = true;

    // A KNOB THAT EDITS THE RUN ANNOUNCES ITSELF AT STARTUP, not only when it fires.
    //
    // It did fire loudly, at Warn, with the counts on both sides of the change -- and
    // it still cost four cats over four launches, because that line was hundreds of
    // lines into a log nobody was reading at the time, and the count that mattered had
    // already happened. Measured 2026-09-22: `"roster_shrink": 1` left in mgmp.json
    // while a different test was being run dropped one cat per process, 4 -> 3 -> 2 ->
    // 1, each drop saved by the game's next checkpoint. So a setting that will delete
    // part of a run gets one line where a reader STARTS, saying what it will do and
    // what to set it to.
    const uint32_t will_drop = config().roster_shrink;
    if (will_drop)
        log_line_lvl(LogLevel::Warn, "ROSTER",
                     "!! config debug.roster_shrink = %u is ENABLED: this process will"
                     " drop %u cat(s) from the run's party at its first map tick, once,"
                     " and the game will save that. Set it to 0 in mgmp.json unless the"
                     " run-editing primitive is what is being tested on purpose.",
                     will_drop, will_drop);

    log_line_lvl(LogLevel::Trace, "ROSTER",
                 "resync fn rva 0x%X verified -- slot %d of the source table can be"
                 " written and re-synced on request (config debug.roster_shrink)",
                 (unsigned)kRva_Resync, (int)kSlotIndex);
}

void roster_set_turn_control(void* turn_control) {
    if (!turn_control) return;
    ++g.turn_serial;
    if (!g.on || turn_control == g.tc) return;
    g.tc = turn_control;
    log_line_lvl(LogLevel::Trace, "ROSTER",
                 "turn control 0x%llX -- the source table behind it is now reachable",
                 (unsigned long long)(uintptr_t)turn_control);
}

// TEMPORARY, READ-ONLY, ONE LINE PER PROCESS (2026-09-22): the 4+4 spike's first question.
//
// The spike appends four more cats to the run's party, and the only safe source of ids is
// ids that ALREADY belong to this run. The party vector (MewDirector+1468/+1472) has a large
// capacity -- measured 660 against a party of 4 -- so the slots beyond `count` may already
// hold real ids (the cat box the House fills) rather than garbage. This prints the party, the
// slots just past it, and the familiar list for reference. Nothing is written and nothing
// downstream may guess: the append gets written against ids this line proves.
//
// Delete this whole block once the append is in -- it is a measurement, not a feature.
void probe_box_once() {
    static bool done = false;
    if (done) return;
    done = true;

    const void** slot = (const void**)((uintptr_t)GetModuleHandleW(nullptr) + kRva_MewDirectorPtr);
    const void*  md   = nullptr;
    if (!mem_read(slot, &md, sizeof(md)) || !md) {
        log_line("ROSTER", "catbox probe: MewDirector not readable yet -- nothing to measure");
        return;
    }

    uint32_t  count = 0;
    uintptr_t data  = 0;
    if (!mem_read((const uint8_t*)md + kDir_CatIdCount, &count, 4) ||
        !mem_read((const uint8_t*)md + kDir_CatIdData, &data, sizeof(data)) || !data) {
        log_line("ROSTER", "catbox probe: +%u/+%u did not read as the cat id vector",
                 (unsigned)kDir_CatIdCount, (unsigned)kDir_CatIdData);
        return;
    }

    // The party, then the twelve slots that follow it. Every id is printed as 16 hex digits
    // because that is the width the vector uses (MAPNODE lessons: the width is 8 bytes, not
    // four) and because an id is the only form of this that another log can be checked
    // against.
    char party[256] = {};
    int  po = 0;
    const uint32_t shown = count < 8 ? count : 8;
    for (uint32_t i = 0; i < shown; ++i) {
        uint64_t id = 0;
        if (!mem_read((const void*)(data + i * 8), &id, 8)) break;
        po += _snprintf_s(party + po, sizeof(party) - po, _TRUNCATE, " %016llx",
                          (unsigned long long)id);
    }

    char beyond[320] = {};
    int  bo = 0;
    for (uint32_t i = count; i < count + 12; ++i) {
        uint64_t id = 0;
        if (!mem_read((const void*)(data + i * 8), &id, 8)) break;
        bo += _snprintf_s(beyond + bo, sizeof(beyond) - bo, _TRUNCATE, " [%u]=%016llx", i,
                          (unsigned long long)id);
    }

    log_line("ROSTER", "catbox probe: party count=%u, entries:%s", count, party);
    log_line("ROSTER", "catbox probe: slots AT AND PAST the count:%s", beyond);

    // The familiar list, whose layout is already proven (kDir_CatFamiliars: count at +4,
    // pointer at +8 -- see cat_is_familiar in mgmp_lockstep): a second, independent id list
    // in the same run, printed so the two can be compared in one log.
    uint32_t        fam_n = 0;
    const uint64_t* fam   = nullptr;
    if (mem_read((const uint8_t*)md + kDir_CatFamiliars + 4, &fam_n, 4) && fam_n && fam_n <= 64 &&
        mem_read((const uint8_t*)md + kDir_CatFamiliars + 8, &fam, sizeof(fam)) && fam) {
        char fams[320] = {};
        int  fo = 0;
        for (uint32_t i = 0; i < fam_n; ++i) {
            uint64_t id = 0;
            if (!mem_read(&fam[i], &id, 8)) break;
            fo += _snprintf_s(fams + fo, sizeof(fams) - fo, _TRUNCATE, " %016llx",
                              (unsigned long long)id);
        }
        log_line("ROSTER", "catbox probe: familiars (%u):%s", fam_n, fams);
    } else {
        log_line("ROSTER", "catbox probe: no familiar list (count did not read, or empty)");
    }
}

void roster_tick() {
    if (!g.on || g.applied) return;

    probe_box_once();          // temporary, read-only; see the note above

    const uint32_t want = config().roster_shrink;
    if (!want) return;                 // 0 = the whole feature is off

    ++g.ticks;

    // WHERE THE PARTY ACTUALLY LIVES, AND WHY SLOT 505 IS NOT IT.
    //
    // The first version of this module edited slot 505 of the source table -- the
    // list the watchpoint instrument had watched being rebuilt. Run against a real
    // session on 2026-09-22 it refused, correctly, with "slot 505 did not read as
    // {cap, count, array} plus a live container", and the refusal was the whole
    // finding: THAT LIST IS BATTLE-SCOPED. It exists while a fight is in progress
    // and is released when the fight ends -- which the instrument had said all
    // along ("a released roster must DISARM, not keep watching a tombstone") and
    // which the disassembly then confirmed from the other end: every writer of it
    // lives in the turn layer (0x8E3420 resyncs seven slots from one body, and the
    // virtual 0x8E2A30 that drives it calls TurnControl::QueueDecision).
    //
    // Derived, not authoritative. What decides who fights is the RUN's cat id
    // vector, which the mod already reads and already prints:
    //
    //     CATSYNC  run cat order (4): 2fc 24d 2f6 2df
    //
    // `MewDirector + 1468` is its count and `+1472` its data pointer -- the same
    // pair adventure_is_loaded_impl reads to answer "is a run loaded", and the same
    // one catsync walks. It is run-level, it survives between battles, and it needs
    // no battle to test.
    //
    // WHAT THIS WRITES IS THEREFORE THE RUN, not a battle copy: the count is
    // decremented and the next battle the game builds is smaller. That is the
    // primitive a per-peer party is made of -- and the honest way to verify it is
    // the roster line of the next battle, not this log.
    const void** slot = (const void**)((uintptr_t)GetModuleHandleW(nullptr) + kRva_MewDirectorPtr);
    const void*  md   = nullptr;
    if (!mem_read(slot, &md, sizeof(md)) || !md) {
        if (!g.said_no_tc) {
            g.said_no_tc = true;
            log_line("ROSTER", "debug.roster_shrink is set but the MewDirector pointer is"
                               " not readable yet -- nothing to edit. This says so once;"
                               " the map tick keeps trying.");
        }
        return;
    }

    uint32_t  count = 0;
    uintptr_t data  = 0;
    if (!mem_read((const uint8_t*)md + kDir_CatIdCount, &count, 4) ||
        !mem_read((const uint8_t*)md + kDir_CatIdData, &data, sizeof(data)) || !data) {
        if (!g.said_no_sub) {
            g.said_no_sub = true;
            log_line("ROSTER", "debug.roster_shrink is set but MewDirector+%u/+%u did not"
                               " read as the run's cat id vector -- nothing to edit"
                               " (offsets drifted?)",
                     (unsigned)kDir_CatIdCount, (unsigned)kDir_CatIdData);
        }
        return;
    }

    // An empty party is not a state the game builds; refuse rather than find out.
    if (count <= 1) {
        log_line_lvl(LogLevel::Warn, "ROSTER",
                     "!! debug.roster_shrink = %u but the run lists %u cat(s) -- nothing"
                     " to drop; leaving the run alone", want, count);
        g.applied = true;
        return;
    }

    // Read the ids out first, so the line can show what was removed and not only
    // how many. The ids are what both peers compare (`run cat order`), so they are
    // the only form of this change anybody can check against another log.
    uint32_t ids[16] = {};
    const uint32_t shown = count < 16 ? count : 16;
    if (!mem_read((const void*)data, ids, shown * 4)) {
        log_line_lvl(LogLevel::Error, "ROSTER",
                     "!! the run's cat id array is not readable -- writing nothing");
        g.applied = true;
        return;
    }

    char before[128] = {}, after[128] = {};
    int off = 0;
    for (uint32_t i = 0; i < shown && off < 100; ++i)
        off += _snprintf_s(before + off, sizeof(before) - off, _TRUNCATE, "%s%x",
                           i ? " " : "", ids[i]);
    for (uint32_t i = 0, n = 0; i < shown && off >= 0; ++i) {
        if (i >= shown - (uint32_t)1 && count > 1) continue;   // the dropped ones
        off += _snprintf_s(after + (int)strlen(after), 128 - (int)strlen(after), _TRUNCATE,
                           "%s%x", n++ ? " " : "", ids[i]);
    }

    const uint32_t after_count = count - ((want < count) ? want : (count - 1));
    // BOTH COUNTS, OR NEITHER (2026-09-23). The engine keeps the run's cat list twice
    // (+1468 and +1484 -- see mgmp_addresses.h), so dropping cats from one and not the
    // other is the same inconsistency the grow path now refuses. This shrink is the
    // project's OWN test rig, which makes it the worst place to leave a desync: the
    // next crash would be blamed on whatever was being tested at the time.
    if (!mem_write((uint8_t*)md + kDir_CatIdCount, &after_count, 4) ||
        !mem_write((uint8_t*)md + kDir_MirrorCount, &after_count, 4)) {
        log_line_lvl(LogLevel::Error, "ROSTER",
                     "!! the run's cat id count is not writable -- the run is untouched");
        g.applied = true;
        return;
    }

    uint32_t check = 0, mirror_check = 0;
    mem_read((const uint8_t*)md + kDir_CatIdCount, &check, 4);
    mem_read((const uint8_t*)md + kDir_MirrorCount, &mirror_check, 4);

    g.last_src_before  = count;  g.last_src_after  = check;
    g.last_live_before = count;  g.last_live_after = check;
    g.last_ok = (check == after_count && mirror_check == after_count);
    g.applied = true;

    log_line_lvl(LogLevel::Warn, "ROSTER",
                 "!! EDITED THE RUN: the party was %u cat(s) [%s] and is now %u [%s] --"
                 " count %u -> %u at MewDirector+%u (mirror +%u reads %u), written and"
                 " read back (%s). THIS"
                 " AFFECTS THE NEXT BATTLE the game builds, not any fight in progress:"
                 " the battle-scoped list is derived from this one at the node boundary."
                 " Verify by the roster line of the next fight.",
                 count, before, after_count, after, count, after_count,
                 (unsigned)kDir_CatIdCount, (unsigned)kDir_MirrorCount, mirror_check,
                 g.last_ok ? "read-back agrees" : "READ-BACK DISAGREES");
}

void roster_status(char* out, size_t out_size) {
    if (!out || !out_size) return;
    if (!g.applied)
        _snprintf_s(out, out_size, _TRUNCATE, "roster: ready (config debug.roster_shrink"
                                              " edits the RUN's party)");
    else
        _snprintf_s(out, out_size, _TRUNCATE, "roster: party %u -> %u (%s)",
                    g.last_src_before, g.last_src_after,
                    g.last_ok ? "written and read back" : "READ-BACK DISAGREED");
}

// ---------------------------------------------------------------------------
// The party exchange.

namespace {

// (The capacity used to be read from 1460, with a note saying +1464's purpose was not
// established. 2026-09-23: +1464 IS the cap -- see the note on kDir_CatIdCap -- and
// that constant now lives in mgmp_addresses.h beside the two fields it belongs to.)

// --- THE GAME'S OWN APPEND, WHICH IS HOW A PARTY GROWS (2026-09-23) ----------------
//
// C_CatIdAppend (0x48010) is already in this project's call table under the name "Cat
// ID vector append", and it is used here rather than reimplemented for a reason that
// is measurable rather than stylistic: it is a vector<u64>::push_back. It reads the cap
// from [rcx] and the count from [rcx+4], reallocates through the game's allocator when
// they are equal (new cap = max(2, 1.5x), `shl rdx,3` for the element size), stores the
// new array, and appends. The game calls it with &MewDirector+0x640 -- the familiars
// list -- from the code that files a new cat there once the party is full
// (0x929E4C: `lea rcx,[rdi+0x640]; call 0x48010`). Two independent things follow:
//
//   * calling it CANNOT write out of bounds, which is exactly what the in-place writes
//     did at six cats (heap corruption, c0000374, FEASIBILITY §10.3);
//   * its first argument is a vector BASE with cap at +0 -- which is a third, external
//     confirmation that MewDirector+1464 is the cap (1468 count, 1472 data), the layout
//     this file only started using tonight.
// Native 0x48082 loads *id into RAX and never replaces it before RET.
// There is no success-pointer contract; appending zero legitimately leaves RAX=0.
using AppendFn = void (__fastcall*)(void*, const uint64_t*);

AppendFn append_fn() {
    static AppendFn fn = (AppendFn)addr_of_call(C_CatIdAppend);
    return fn;
}

// Judge the mutation by vector readback, never by an unspecified return value.
bool append_checked(AppendFn append, void* vec, const uint64_t* id) {
    uint32_t before=0,cap=0,after=0;uint64_t* data=nullptr;uint64_t stored=0;
    if(!append||!vec||!id||!mem_read(vec,&cap,4)||
       !mem_read((uint8_t*)vec+4,&before,4)||before>cap||before>=64)return false;
    __try { append(vec,id); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    return mem_read(vec,&cap,4)&&mem_read((uint8_t*)vec+4,&after,4)&&
           after==before+1&&after<=cap&&mem_read((uint8_t*)vec+8,&data,sizeof(data))&&data&&
           mem_read(data+before,&stored,sizeof(stored))&&stored==*id;
}

bool same_list(const uint32_t* a, uint32_t an, const uint32_t* b, uint32_t bn) {
    if (an != bn) return false;
    for (uint32_t i = 0; i < an; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

void ids_text(const uint32_t* ids, uint32_t n, char* out, size_t cap) {
    int off = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < n && off < (int)cap - 8; ++i)
        off += _snprintf_s(out + off, cap - off, _TRUNCATE, "%s%x", i ? " " : "", ids[i]);
}

// The run's list, as it stands. False when there is no run, or when the offsets
// stopped reading -- in both cases the caller does nothing at all.
bool read_party(uint32_t* out, uint32_t& n) {
    const void** slot = (const void**)((uintptr_t)GetModuleHandleW(nullptr) + kRva_MewDirectorPtr);
    const void*  md   = nullptr;
    if (!mem_read(slot, &md, sizeof(md)) || !md) return false;

    uint32_t  count = 0;
    uintptr_t data  = 0;
    if (!mem_read((const uint8_t*)md + kDir_CatIdCount, &count, 4)) return false;
    if (!mem_read((const uint8_t*)md + kDir_CatIdData, &data, sizeof(data)) || !data) return false;
    if (count == 0 || count > kMaxPartyIds) { n = 0; return count == 0; }

    // Stride 8, id in the low four. Read the whole 8 bytes per element so the
    // stride is not a guess: the high half is carried along, not assumed zero.
    uint8_t raw[kMaxPartyIds * 8] = {};
    if (!mem_read((const void*)data, raw, count * 8)) return false;
    for (uint32_t i = 0; i < count; ++i)
        memcpy(&out[i], raw + i * 8, 4);
    n = count;
    return true;
}

// THE EDITOR SWAP'S FENCE (2026-09-23, route A). While the gear screen is open this
// peer's party holds its OWN cats and the familiars hold the other side's (see
// party_swap_tick). This function is the one place the party count is written, so it is
// also where an incoming agreed list has to be refused for that duration -- otherwise a
// PUBLISH lands mid-edit and quietly undoes what the player is looking at.
static bool g_swap_holds = false;
static bool g_setup_ready = true;

// Write it. Only the count field that was measured, only inside the measured
// capacity, and every value is read back before anybody is told it happened.
bool write_party(const uint32_t* ids, uint32_t n, const char* why, char* before,
                 size_t before_cap) {
    if (n == 0 || n > kMaxPartyIds) return false;
    if (g_swap_holds) {
        log_line("ROSTER", "an agreed list of %u cat(s) was NOT written: this peer is in"
                           " the middle of the gear-editor swap (the screen is open), and"
                           " the list it is showing is its own", n);
        return false;
    }

    const void** slot = (const void**)((uintptr_t)GetModuleHandleW(nullptr) + kRva_MewDirectorPtr);
    const void*  md   = nullptr;
    if (!mem_read(slot, &md, sizeof(md)) || !md) return false;

    uint32_t  count = 0, cap = 0, mirror_count = 0, mirror_cap = 0;
    uintptr_t data  = 0, mirror_data = 0;
    if (!mem_read((const uint8_t*)md + kDir_CatIdCount, &count, 4)) return false;
    if (!mem_read((const uint8_t*)md + kDir_CatIdCap,   &cap,   4)) return false;
    if (!mem_read((const uint8_t*)md + kDir_CatIdData,  &data, sizeof(data)) || !data)
        return false;
    if (!mem_read((const uint8_t*)md + kDir_MirrorCount, &mirror_count, 4)) return false;
    if (!mem_read((const uint8_t*)md + kDir_MirrorCap,   &mirror_cap,   4)) return false;
    if (!mem_read((const uint8_t*)md + kDir_MirrorData,  &mirror_data, sizeof(mirror_data)))
        return false;

    // THE TWO LISTS, ON THE RECORD, BEFORE ANY CHECK (2026-09-23). The mirror is new:
    // the engine keeps the run's cat list twice, sixteen bytes apart, and writes both
    // at departure (probe log, 11:28). Printed before the checks so a refusal reports
    // the same numbers a success does -- the one reading that matters most is the one
    // a refusal would otherwise omit.
    log_line("ROSTER", "the run's cat-id lists: party {count %u, cap %u} and its mirror"
                       " {count %u, cap %u} (%s) -- a 4+4 party needs both to hold 8",
             count, cap, mirror_count, mirror_cap, why);

    // What we are replacing, in a LOCAL buffer: this used to read straight into
    // g.my_ids, which then made the publisher at the end of the tick think its own
    // list had changed and send a second, redundant PARTY message. Harmless (the
    // content was identical) and bounded (one extra round trip per write), but it
    // doubled the log lines and the sends for no information at all.
    uint32_t prev[kMaxPartyIds] = {};
    uint32_t prev_n = 0;
    if (count) read_party(prev, prev_n);
    ids_text(prev, prev_n, before, before_cap);

    // --- THE WRITE: in place what is already allocated, the GAME'S OWN append for
    // the rest (2026-09-23).
    //
    // This used to write every id in place and then set the count, and it was gated by
    // a capacity that was read from the WRONG offset (+1460, see kDir_CatIdCap). At six
    // cats that gate passed and the ids went one past the end of a four-element
    // allocation: heap corruption, `c0000374`, reported later on the quit path
    // (FEASIBILITY §10.3/§10.2). Refusing instead was the first fix and it is what the
    // log showed at 11:37 -- correct, but it also means a 4+4 party can never be built.
    //
    // The real answer was already in this project's own call table: C_CatIdAppend,
    // 0x48010, "Cat ID vector append". It is a vector<u64>::push_back -- it reads cap
    // from [rcx] and count from [rcx+4], reallocates through the game's allocator when
    // the two are equal, and appends. The game itself calls it with &MewDirector+0x640
    // (the familiars list) exactly when a new cat arrives and the party is full
    // (0x929E4C: `lea rcx,[rdi+0x640]; call 0x48010`), which is what makes it safe to
    // call: no in-place write can go out of bounds through it, and the growth policy is
    // the engine's own rather than one this module invented.
    //
    // So: the ids that already have slots are written in place, and every id PAST the
    // current count is appended. Nothing sets a count directly on the grow path -- the
    // append does that -- and a shrink still writes the count, because a count can
    // only be lowered by writing it.
    // THE ENGINE'S OWN LIMIT, MEASURED (2026-09-23): a run's party is an exactly-four
    // thing. Six cats in it stop the map layer within seconds -- an access violation at
    // exe+0x99A792, identical on both peers, with the heap intact (the c0000374
    // corruption of FEASIBILITY §10.3 is gone since this module started growing through
    // the engine's own append). So this module refuses to grow past four: extra cats
    // belong in the FAMILIARS list, which is the engine's own slot for them and which
    // DOES fight -- measured, 4 party + 4 familiars = "8 human, peer 0:4 peer 1:4",
    // both peers, hashes agreeing. See FEASIBILITY §11.
    if (n > 4) {
        log_line_lvl(LogLevel::Error, "ROSTER",
                     "!! refusing a %u-cat party: a run's party is four cats in this"
                     " engine -- six of them crash the map layer at exe+0x99A792"
                     " (measured twice on 2026-09-23, both peers, same stack). Extra"
                     " cats go in the FAMILIARS list; see FEASIBILITY §11.", n);
        return false;
    }

    if ((count && !data) || (mirror_count && !mirror_data)) {
        log_line_lvl(LogLevel::Error, "ROSTER",
                     "!! the run lists %u cat(s) but one of its two arrays is null --"
                     " NOT writing anything (a list without an array is a state this"
                     " module will not build on)", count);
        return false;
    }

    AppendFn append = append_fn();
    if (!append) {
        log_line_lvl(LogLevel::Error, "ROSTER",
                     "!! C_CatIdAppend did not resolve on this build -- NOT writing"
                     " anything. A party longer than the current capacity needs the"
                     " game's own append; writing in place instead is what corrupted the"
                     " heap at six cats.");
        return false;
    }

    const uint32_t keep       = (count < n) ? count : n;             // already allocated
    const uint32_t mirror_keep = (mirror_count < n) ? mirror_count : n;

    for (uint32_t i = 0; i < n; ++i) {
        uint64_t v = ids[i];                          // id in the low four, pad zero

        if (i < keep) {
            if (!mem_write((uint8_t*)data + i * 8, &v, 8)) {
                log_line_lvl(LogLevel::Error, "ROSTER",
                             "!! the party array is not writable at index %u -- the run"
                             " is now in an UNKNOWN state; do not trust this run", i);
                return false;
            }
        } else if (!append_checked(append, (void*)((uint8_t*)md + kDir_CatIdCap), &v)) {
            log_line_lvl(LogLevel::Error, "ROSTER",
                         "!! the game's own append refused the party's id %u (index %u)"
                         " -- the run is now in an UNKNOWN state; do not trust this run",
                         ids[i], i);
            return false;
        }

        if (i < mirror_keep) {
            if (!mem_write((uint8_t*)mirror_data + i * 8, &v, 8)) {
                log_line_lvl(LogLevel::Error, "ROSTER",
                             "!! the mirror array is not writable at index %u -- the run"
                             " is now in an UNKNOWN state; do not trust this run", i);
                return false;
            }
        } else if (!append_checked(append, (void*)((uint8_t*)md + kDir_MirrorCap), &v)) {
            log_line_lvl(LogLevel::Error, "ROSTER",
                         "!! the game's own append refused the mirror's id %u (index %u)"
                         " -- the run is now in an UNKNOWN state; do not trust this run",
                         ids[i], i);
            return false;
        }
    }

    // A SHRINK still writes both counts; a grow was done by the appends above. The
    // growth is reported because it is new: this path has never run before.
    if (n < count || n < mirror_count) {
        const uint32_t smaller = n;
        if (!mem_write((uint8_t*)md + kDir_CatIdCount, &smaller, 4) ||
            !mem_write((uint8_t*)md + kDir_MirrorCount, &smaller, 4)) {
            log_line_lvl(LogLevel::Error, "ROSTER",
                         "!! a party count is not writable -- the run may be inconsistent");
            return false;
        }
        log_line("ROSTER", "the party was SHORTENED %u -> %u in both lists (the count is"
                           " the only thing a shrink has to write)", count, n);
    } else if (n > count) {
        // The caps are re-read rather than reused: the appends above are the thing that
        // may have changed them, and a log that printed the value from before the
        // growth twice would be the kind of number a reader later trusts by mistake.
        uint32_t party_cap_after = 0, mirror_cap_after = 0;
        mem_read((const uint8_t*)md + kDir_CatIdCap,  &party_cap_after,  4);
        mem_read((const uint8_t*)md + kDir_MirrorCap, &mirror_cap_after, 4);
        log_line_lvl(LogLevel::Warn, "ROSTER",
                     "!! GREW THE RUN'S PARTY with the game's own append (C_CatIdAppend"
                     " 0x48010): %u -> %u cat(s), party cap %u -> %u, mirror %u -> %u"
                     " cat(s), mirror cap %u -> %u. This write has never been safe"
                     " before -- it is the engine's reallocation policy, not one this"
                     " module invented.", count, n, cap, party_cap_after, mirror_count, n,
                     mirror_cap, mirror_cap_after);
    }

    uint32_t check = 0, mirror_check = 0;
    uint32_t back[kMaxPartyIds] = {};
    uint32_t back_n = 0;
    mem_read((const uint8_t*)md + kDir_CatIdCount, &check, 4);
    mem_read((const uint8_t*)md + kDir_MirrorCount, &mirror_check, 4);
    read_party(back, back_n);

    char after[128] = {};
    ids_text(back, back_n, after, sizeof(after));

    const bool same = (check == n && mirror_check == n && same_list(back, back_n, ids, n));

    log_line_lvl(LogLevel::Warn, "ROSTER",
                 "!! PARTY AGREED (%s): [%s] -> [%s], count %u -> %u (+mirror %u),"
                 " read back %s. This is the run's own cat id list -- the layer that"
                 " decides who fights -- so the next battle on BOTH peers is built"
                 " from it, and its mirror is kept in step so the engine's own paths"
                 " (the one that crashed on quit) see one list, not two.",
                 why, before, after, count, check, mirror_check,
                 same ? "identical" : "!! DIFFERENT");
    return true;
}

} // namespace

void roster_party_tick() {
    if (!net_active()) return;
    // The chapter setup barrier installs a complete, namespaced 4+4 roster.
    // The older PARTY exchange only knows how to union two ordinary party
    // lists; running it here turns the setup roster into an illegal six/eight
    // cat party and floods the log every frame.
    if (tune::kLocalSetup) return;

    // While the gear-editor swap holds, this peer's party is its own list, not the run's
    // shared one -- so it must not be offered to the other peer (see party_swap_tick).
    if (g_swap_holds) return;

    uint32_t run[kMaxPartyIds] = {};
    uint32_t run_n = 0;
    if (!read_party(run, run_n) || run_n == 0) return;

    // --- MY OWN HALF OF THE PARTY (2026-09-22, 4+4 steps 1-2) --------------------
    //
    // This used to publish the WHOLE run list. That is only meaningful while both peers
    // load the same save, so both said the same thing and the merge was a no-op. The 4+4
    // shape needs the same channel to carry something else: WHICH OF THESE CATS ARE MINE.
    // The merge below is untouched -- peer 0's order first, then the other peer's new
    // ones -- because ACTION is index-keyed and the ORDER is as load-bearing as the ids;
    // two cats from each side come out of it as one four-cat party both peers agree on.
    //
    // The rule, and why it is not symmetric (see mgmp_roster.h for the full statement):
    //
    //   provably mine       publish it
    //   provably the peer's do NOT publish: those ids arrive over the wire, verified,
    //                       and this module never invents an id (the catbox probe of
    //                       2026-09-22 proved the slots past the party are garbage)
    //   not known yet       publish it anyway -- before the first battle nothing is
    //                       provable, and then this reduces to the old behaviour exactly
    uint32_t mine[kMaxPartyIds] = {};
    uint32_t n = 0;
    uint32_t theirs  = 0;
    uint32_t unknown = 0;
    for (uint32_t i = 0; i < run_n && n < kMaxPartyIds; ++i) {
        bool is_mine = false;
        const bool known = lockstep_cat_is_mine((uint64_t)run[i], is_mine);
        if (known && !is_mine) { ++theirs; continue; }
        if (!known) ++unknown;
        mine[n++] = run[i];
    }
    if (n == 0) return;                       // nothing to claim, so nothing to send

    if (theirs && g.my_n == 0) {
        // The first tick this peer can tell its cats from the other player's. At Warn,
        // because it changes what the next battle is built from.
        char all[128] = {}, own[128] = {};
        ids_text(run, run_n, all, sizeof(all));
        ids_text(mine, n, own, sizeof(own));
        log_line_lvl(LogLevel::Warn, "ROSTER",
                     "!! OWNERSHIP KNOWN: the run's party [%s] is mine [%s] plus %u cat(s)"
                     " the other player owns. From here this peer publishes only its own"
                     " half, and the merge assembles the two halves in peer 0's order --"
                     " that list is the layer that decides who fights.", all, own, theirs);
    }

    // Our own picks, whenever they change -- that is what "each player prepares
    // their own cats" means: the cat box writes the run's list, and this notices.
    if (!same_list(mine, n, g.my_ids, g.my_n)) {
        memcpy(g.my_ids, mine, sizeof(uint32_t) * n);
        g.my_n = n;

        char text[128] = {};
        ids_text(mine, n, text, sizeof(text));

        PartyMsg m{};
        m.kind  = 0;
        m.count = (uint8_t)n;
        for (uint32_t i = 0; i < n; ++i) m.ids[i] = mine[i];
        if (net_send_party(m))
            log_line("ROSTER", "-> PARTY (mine): [%s] -- sent to the room (%u of the run's"
                               " %u cat(s) are the other player's, %u not yet attributed)",
                     text, theirs, run_n, unknown);
        else
            log_line("ROSTER", "!! could not send my party [%s] -- the merge cannot"
                               " happen until a send succeeds", text);
        return;   // one change per tick
    }

    // Peer 0 merges. Everyone else waits for kind 1 and writes that.
    if (net_role() != NetRole::Host) return;
    if (g.peer_n == 0) return;

    uint32_t merged[kMaxPartyIds] = {};
    uint32_t mn = 0;
    for (uint32_t i = 0; i < n && mn < kMaxPartyIds; ++i) merged[mn++] = mine[i];
    for (uint32_t i = 0; i < g.peer_n && mn < kMaxPartyIds; ++i) {
        bool have = false;
        for (uint32_t j = 0; j < mn && !have; ++j) have = (merged[j] == g.peer_ids[i]);
        if (!have) merged[mn++] = g.peer_ids[i];
    }

    if (same_list(merged, mn, g.agreed, g.agreed_n) && g.said_agree) return;
    if (!same_list(merged, mn, mine, n)) {
        char before[128] = {};
        if (!write_party(merged, mn, "merged with the other peer's picks", before,
                         sizeof(before)))
            return;
    }

    memcpy(g.agreed, merged, sizeof(uint32_t) * mn);
    g.agreed_n = mn;
    g.said_agree = true;

    PartyMsg m{};
    m.kind  = 1;
    m.count = (uint8_t)mn;
    for (uint32_t i = 0; i < mn; ++i) m.ids[i] = merged[i];
    if (net_send_party(m)) {
        char text[128] = {};
        ids_text(merged, mn, text, sizeof(text));
        log_line("ROSTER", "-> PARTY (agreed): [%s] -- every peer writes this same list",
                 text);
    }
}

// --- T1 (2026-09-23): THE PEER'S CATS INTO THE FAMILIARS LIST ----------------------
//
// The party is an exactly-four thing in this engine -- six cats in it crash the map
// layer with an access violation at exe+0x99A792, identical on both peers, seconds
// after the write -- while the engine's OWN answer to "this cat cannot join the party"
// is to file it as a familiar (0x929E4C: `lea rcx,[rdi+0x640]; call 0x48010`). So the
// shape being tested is: each peer's own cats in the party, the other peer's cats in
// the familiars list.
//
// This is the same append write_party now uses, aimed at +0x640 instead of +0x1464:
// nothing new is invented here, and the question is deliberately narrow -- does the map
// layer tolerate a familiar list filled from the wire? The party is not touched at all.
bool roster_add_familiars(const uint64_t* ids, uint32_t count, const char* why) {
    if (!ids || !count || count > 64) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!ids[i] || ids[i] == UINT64_MAX) return false;

    const void** slot = (const void**)((uintptr_t)GetModuleHandleW(nullptr) + kRva_MewDirectorPtr);
    const void*  md   = nullptr;
    if (!mem_read(slot, &md, sizeof(md)) || !md) {
        log_line_lvl(LogLevel::Error, "ROSTER",
                     "!! %s: the MewDirector is not readable -- nothing added", why);
        return false;
    }

    AppendFn append = append_fn();
    if (!append) {
        log_line_lvl(LogLevel::Error, "ROSTER",
                     "!! C_CatIdAppend did not resolve on this build -- nothing added to"
                     " the familiars list");
        return false;
    }

    const uint8_t* vec = (const uint8_t*)md + kDir_CatFamiliars;
    uint32_t  cap = 0, n = 0;
    uintptr_t data = 0;
    if (!mem_read(vec, &cap, 4) || !mem_read(vec + 4, &n, 4) ||
        !mem_read(vec + 8, &data, sizeof(data)) || n > cap || n > 64 || (n && !data)) {
        log_line_lvl(LogLevel::Error, "ROSTER",
                     "!! the familiars vector did not read at MewDirector+%u -- nothing"
                     " added. (mgmp_choice.h names +1576 for this; the note in"
                     " mgmp_addresses.h says +0x640 and warns the two disagree, so this"
                     " line is the measurement.)", (unsigned)kDir_CatFamiliars);
        return false;
    }

    log_line("ROSTER", "%s: the familiars list before -- count %u, cap %u, data 0x%llX",
             why, n, cap, (unsigned long long)data);

    const uint32_t before_n = n;
    const uintptr_t before_data = data;
    uint32_t added = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t v = ids[i];
        if (!v) continue;

        // Re-read every time: an append may reallocate, and a stale data pointer would
        // make the duplicate check read freed memory.
        uint32_t current_cap = 0;
        if (!mem_read(vec, &current_cap, 4) || !mem_read(vec + 4, &n, 4) ||
            !mem_read(vec + 8, &data, sizeof(data)) || n > current_cap || n > 64 || (n && !data)) {
            log_line_lvl(LogLevel::Error, "ROSTER",
                         "!! the familiars vector stopped reading mid-write (%s)", why);
            return false;
        }

        bool dup = false;
        for (uint32_t j = 0; j < n && !dup; ++j) {
            uint64_t have = 0;
            if (!mem_read((const uint8_t*)data + j * 8, &have, 8)) return false;
            if (have == v) dup = true;
        }
        if (dup) continue;

        if (n >= 64 || !append_checked(append, (void*)vec, &v)) {
            log_line_lvl(LogLevel::Error, "ROSTER",
                         "!! the game's own append refused familiar id %llx -- the"
                         " familiars list is now in an UNKNOWN state; do not trust this"
                         " run", (unsigned long long)v);
            return false;
        }
        ++added;
    }

    uint32_t  after_cap = 0, after_n = 0;
    uintptr_t after_data = 0;
    if (!mem_read(vec, &after_cap, 4) || !mem_read(vec + 4, &after_n, 4) ||
        !mem_read(vec + 8, &after_data, sizeof(after_data)) || after_n > after_cap ||
        after_n > 64 || (after_n && !after_data)) return false;

    // Success means every requested id is present, including an import already
    // stored in a resumed save. Counting only appends retried the import every
    // map tick, repeatedly loading the original cat bytes over current progress.
    uint64_t verified[64]{};
    for (uint32_t j = 0; j < after_n; ++j)
        if (!mem_read((const uint8_t*)after_data + j * 8, &verified[j], 8)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        bool present = false;
        for (uint32_t j = 0; j < after_n; ++j) present |= verified[j] == ids[i];
        if (!present) {
            log_line_lvl(LogLevel::Error, "ROSTER", "!! %s: familiar %llx missing on readback",
                         why, (unsigned long long)ids[i]);
            return false;
        }
    }

    // The ids, read back out of the vector rather than repeated from the input: this is
    // the line that says whether the append actually stored what it was given.
    char text[192] = {};
    int  off = 0;
    for (uint32_t j = 0; j < after_n && off < (int)sizeof(text) - 24; ++j) {
        uint64_t have = 0;
        if (!mem_read((const uint8_t*)after_data + j * 8, &have, 8)) break;
        off += _snprintf_s(text + off, sizeof(text) - off, _TRUNCATE, "%s%llx",
                          j ? " " : "", (unsigned long long)have);
    }

    log_line_lvl(LogLevel::Warn, "ROSTER",
                 "!! T1: %s -- %u id(s) appended to the FAMILIARS list (MewDirector+%u):"
                 " count %u -> %u, cap %u -> %u, data 0x%llX -> 0x%llX. Read back: [%s]."
                 " The party list was NOT touched, so if the map survives this, the"
                 " party-at-four-plus-familiars shape is viable.",
                 why, added, (unsigned)kDir_CatFamiliars, before_n, after_n, cap, after_cap,
                 (unsigned long long)before_data, (unsigned long long)after_data, text);
    return true;
}

// Reserve through the native allocator; never write beyond a vector's capacity.
static bool reserve_ids(void* vec,uint32_t need) {
    uint32_t cap=0,n=0; uintptr_t data=0;
    if(!mem_read(vec,&cap,4)||!mem_read((uint8_t*)vec+4,&n,4)||n>cap||n>64) {
        log_line_lvl(LogLevel::Error,"ROSTER",
            "shared roster: malformed vector header at %p (need %u)",vec,need);
        return false;
    }
    // A vector with spare capacity must still have a valid backing allocation.
    // This matters for variable parties: a save can leave an empty familiar
    // vector with a non-zero capacity after a previous run. Calling the native
    // append in that state would write through a null pointer; accepting it and
    // failing later would also leave the transaction half-applied.
    if(cap>=need) {
        if(!need) return true;
        if(!mem_read((uint8_t*)vec+8,&data,8)||!data) {
            log_line_lvl(LogLevel::Error,"ROSTER",
                "shared roster: vector at %p has cap %u/count %u but no data (need %u)",
                vec,cap,n,need);
            return false;
        }
        return true;
    }
    auto append=append_fn(); if(!append) return false;
    uint64_t zero=0;
    bool ok=true;
    for(uint32_t i=n;i<need && ok;++i) ok=append_checked(append,vec,&zero);
    const bool restored=mem_write((uint8_t*)vec+4,&n,4);
    const bool verified=ok && restored && mem_read(vec,&cap,4) && cap>=need;
    if(!verified) log_line_lvl(LogLevel::Error,"ROSTER",
        "native vector reserve failed: saved count=%u requested capacity=%u read capacity=%u count restored=%s",
        n,need,cap,restored?"yes":"NO");
    return verified;
}

// Transactionally replace the party, companion and mirror vectors. Allocation
// changes may survive rollback; their contents/counts may not.
static bool replace_three(const uint32_t* party,uint32_t pn,const uint32_t* fam,uint32_t fn) {
    uintptr_t dir=0;
    if(!pn || pn>4 || fn>16 || !mem_read((void*)addr_of_data(D_MewDirectorPtr),&dir,8)||!dir) return false;
    struct V { void* vec; uintptr_t data; uint32_t n, want_n; uint64_t old[16], want[16]; } v[3]{};
    const uintptr_t offsets[]={kDir_CatIdCap,kDir_CatFamiliars,kDir_MirrorCap};
    for(unsigned j=0;j<3;++j) {
        auto& e=v[j]; e.vec=(uint8_t*)dir+offsets[j];e.want_n=j==1?fn:pn;
        const uint32_t* src=j==1?fam:party;
        // The party and mirror only need this peer's selected count.  The old
        // 4-slot reservation was inherited from the fixed 4+4 prototype and
        // needlessly forced a native reallocation for Host3/Client2 (or any
        // other partial selection) before the transaction even wrote a value.
        // Reserve the familiar vector for the actual remote count instead.
        const uint32_t reserve_n=j==1?fn:e.want_n;
        if(!reserve_ids(e.vec,reserve_n) || !mem_read((uint8_t*)e.vec+4,&e.n,4) || e.n>16 ||
           !mem_read((uint8_t*)e.vec+8,&e.data,8) ||
           ((e.n || e.want_n) && !e.data) ||
           (e.n && !mem_read((void*)e.data,e.old,e.n*8))) {
            log_line_lvl(LogLevel::Error,"ROSTER",
                "shared roster: cannot read vector %u (reserve %u, count %u, want %u, data 0x%llX)",
                j,reserve_n,e.n,e.want_n,(unsigned long long)e.data);
            return false;
        }
        for(unsigned i=0;i<e.want_n;++i)e.want[i]=src[i];
    }
    bool ok=true;
    for(auto& e:v) {
        if(e.n==e.want_n && !memcmp(e.old,e.want,e.n*8)) continue;
        uint64_t check[16]{};uint32_t cn=0;
        if(!mem_write((void*)e.data,e.want,e.want_n*8)||!mem_write((uint8_t*)e.vec+4,&e.want_n,4)||
           !mem_read((void*)e.data,check,e.want_n*8)||memcmp(check,e.want,e.want_n*8)||
           !mem_read((uint8_t*)e.vec+4,&cn,4)||cn!=e.want_n) {ok=false;break;}
    }
    if(!ok) {
        bool rollback=true;
        for(auto& e:v) {
            uint64_t check[16]{};uint32_t cn=0;
            const bool restored=mem_write((void*)e.data,e.old,e.n*8)&&mem_write((uint8_t*)e.vec+4,&e.n,4)&&
                mem_read((void*)e.data,check,e.n*8)&&!memcmp(check,e.old,e.n*8)&&
                mem_read((uint8_t*)e.vec+4,&cn,4)&&cn==e.n;
            rollback=restored&&rollback;
        }
        log_line_lvl(LogLevel::Error,"ROSTER","roster transaction refused; rollback=%s",rollback?"verified":"FAILED");
    }
    return ok;
}
bool roster_setup_replace_party(const uint64_t* ids,uint32_t count,const char* why) {
    if(!ids || !count || count>4)return false;
    uint32_t low[4]{};
    for(uint32_t i=0;i<count;++i){
        if(!ids[i]||ids[i]>=UINT32_MAX)return false;
        for(unsigned j=0;j<i;++j)if(ids[j]==ids[i])return false;
        low[i]=(uint32_t)ids[i];
    }
    char before[128]{};return write_party(low,count,why,before,sizeof(before));
}
void roster_setup_set_ready(bool ready) {
    g_setup_ready=ready;
    if(!ready)roster_party_swap_reset();
}
bool roster_setup_install_shared(const uint64_t* ids,uint32_t count,const char* why) {
    if(!ids || count>16)return false;
    uint32_t party[4]{},fam[16]{},pn=0,fn=0,owned[kMaxPeers]{};
    for(unsigned i=0;i<count;++i) {
        const int owner=session_cat_owner(ids[i]);
        if(owner<0 || owner>=net_peer_count() || ++owned[owner]>4)return false;
        for(unsigned j=0;j<i;++j)if(ids[i]==ids[j])return false;
        if(owner==0)party[pn++]=(uint32_t)ids[i];else fam[fn++]=(uint32_t)ids[i];
    }
    for(unsigned p=0;p<net_peer_count();++p)if(!owned[p])return false;
    if(!replace_three(party,pn,fam,fn))return false;
    for(unsigned i=0;i<count;++i)lockstep_note_owner(ids[i],(uint8_t)session_cat_owner(ids[i]));
    log_line("ROSTER","shared roster installed: %u host cats + %u client cats (%s)",pn,fn,why);
    return true;
}

// --- ROUTE A: THE GEAR SCREEN, AIMED AT THE WHOLE RUN (2026-09-23) -----------------
//
// See the header for the measurement this rests on: the screen reads its list through
// ONE pointer (+0x10 of the button's callable), and that pointer was captured live.
//
// The stand-in is a COPY of the live object's first kStubBytes with two fields
// overridden -- never a zeroed buffer. The screen reads more through this pointer than
// the two fields we care about, and a zero where a live pointer belongs is how a
// redirect turns into a crash.
//
// The list it answers with is the run's WHOLE cat list: the party, then every familiar
// that is not already in it. That keeps the redirect symmetric -- no peer has to be told
// whether it is the host -- and it is what lets the client reach its own cats in the
// game's own screen. Ownership rules still decide whose edit sticks. The run's party
// vector is never written, nothing is published, and the pointer goes back the moment
// the screen is gone.
static constexpr uintptr_t kBtn_Name_     = 504;
static constexpr uintptr_t kBtn_Callback_ = 240;
static constexpr uintptr_t kStubBytes     = 0x2000;
static constexpr uintptr_t kCallable_List = 0x10;   // the pointer the capture proved
static constexpr uint32_t  kScreenGoneMs  = 400;
static constexpr uint32_t  kMaxStubIds    = 32;
static constexpr uint32_t  kMaxRedirects  = 16;

// EVERY BUTTON OF THE SCREEN HAS ITS OWN CLOSURE, and the list pointer lives inside each
// of them. The first version patched only the first one it recognised and keyed its
// "already aimed" test on that same callable -- so as soon as the detour moved on to the
// next button the redirect was taken away again (8113 installs, measured), and the button
// whose handler actually reads the list may never have been patched at all. Hence a table:
// each distinct callable on the screen is patched once, and all of them are put back
// together.
struct Redirect {
    struct Entry { void* callable = nullptr; uintptr_t saved = 0; };
    Entry     entry[kMaxRedirects] = {};
    uint32_t  entry_n     = 0;
    uint32_t  seen_ms     = 0;
    bool      said_short  = false;
    bool      said_notlist = false;
    bool      active      = false;
    uint8_t   stub[kStubBytes] = {};
    uint64_t  ids[kMaxStubIds] = {};
};
static Redirect g_red;

// The screen this is for, and the button that OPENS it. The opener is in the list on
// purpose: the screen may read the pointer once, when it is built, so waiting until its
// own buttons are updating can be a frame too late -- and the opener is visible on the
// map, i.e. before the click.
static bool name_is_gear_screen(const char* n) {
    return strstr(n, "CatSelector_")    != nullptr ||
           strstr(n, "InventoryButton_") != nullptr ||
           strstr(n, "EquippedButton_")  != nullptr ||
           strcmp(n, "Map_Backpack")     == 0;
}

// The run's whole cat list, party first, then the familiars that are not already in it.
static uint32_t collect_all_ids(uint64_t* out, uint32_t cap) {
    const void** slot = (const void**)((uintptr_t)GetModuleHandleW(nullptr) + kRva_MewDirectorPtr);
    const void*  md   = nullptr;
    if (!mem_read(slot, &md, sizeof(md)) || !md) return 0;

    uint32_t  pn = 0, fn = 0;
    uintptr_t pd = 0, fd = 0;
    if (!mem_read((const uint8_t*)md + kDir_CatIdCount, &pn, 4)) return 0;
    if (!mem_read((const uint8_t*)md + kDir_CatIdData, &pd, sizeof(pd)) || !pd) return 0;
    mem_read((const uint8_t*)md + kDir_CatFamiliars + 4, &fn, 4);
    mem_read((const uint8_t*)md + kDir_CatFamiliars + 8, &fd, sizeof(fd));

    uint32_t n = 0;
    for (uint32_t i = 0; i < pn && n < cap; ++i) {
        uint64_t id = 0;
        if (mem_read((const uint8_t*)pd + i * 8, &id, 8) && id) out[n++] = id;
    }
    if (!fd) return n;
    for (uint32_t i = 0; i < fn && n < cap; ++i) {
        uint64_t id = 0;
        if (!mem_read((const uint8_t*)fd + i * 8, &id, 8) || !id) continue;
        bool dup = false;
        for (uint32_t j = 0; j < n && !dup; ++j) dup = (out[j] == id);
        if (!dup) out[n++] = id;
    }
    return n;
}

static void redirect_restore() {
    if (!g_red.active) return;
    uint32_t back = 0;
    for (uint32_t i = 0; i < g_red.entry_n; ++i) {
        if (g_red.entry[i].callable && g_red.entry[i].saved &&
            mem_write((uint8_t*)g_red.entry[i].callable + kCallable_List,
                      &g_red.entry[i].saved, sizeof(g_red.entry[i].saved)))
            ++back;
    }
    log_line("ROSTER", "route A: %u list pointer(s) BACK on the run's party vector (they"
                       " answered with the whole run while that screen was up)", back);
    g_red.entry_n = 0;
    g_red.active  = false;
}

// --- ROUTE A, WITH REAL STATE INSTEAD OF A STAND-IN (2026-09-23) -------------------
//
// What went wrong with the stand-in is in tune::kPartySwapWhileEditing: the screen writes
// through the pointer it reads, so a copy of that object ate the writes. This version
// changes only REAL state, and only the two vectors' CONTENTS: party 4 <-> familiars 4,
// counts untouched, nothing grown, nothing published while it is held.
//
// The peer is protected from being talked out of it in three places: write_party refuses
// while the swap holds (so the incoming agreed list cannot undo it), roster_party_tick
// does not publish (so this peer does not offer the swapped list to the other one), and
// the swap is released the moment the screen stops updating.
static constexpr uint32_t kSwapIds = 16;

struct EditSwap {
    bool     active = false;
    bool     captured = false;
    bool     waiting_reported = false;
    uintptr_t director = 0;
    uint32_t pn=0,fn=0,ln=0,on=0;
    uint32_t local[16]{},others[16]{};
    uint32_t party_before[kSwapIds] = {};
    uint32_t fam_before[kSwapIds] = {};
    uint32_t mirror_before[kSwapIds] = {};
};
static EditSwap g_swap;
// Once the game has returned to the menu it tears down the 4+4 director
// vectors.  Do not retry a write against that dead shape every frame; the
// next run calls roster_party_swap_reset() and re-enables the feature.
static bool g_swap_shape_invalid = false;
// (g_swap_holds itself is declared ABOVE, next to write_party: that is the one place that
// writes the party count, and therefore the one place that has to know about the swap.)

// THE MAP PHASE IS WHEN THE SWAP LIVES (2026-09-23, revised after the user's report).
//
// It used to be the gear screen's own buttons, i.e. swap-on-open. The player saw the
// consequence immediately: the frame the screen opened still showed the run's four, and
// only a click on the selector made the client's own cats appear. Moving the trigger from
// a UI event to a GAME PHASE fixes that by construction -- the swap is in place before any
// screen can be opened, because it is in place for the whole time the map is up.
//
// Map_Backpack is the map's own button, so its presence IS "this peer is on the map". The
// restore end of the same rule is in party_swap_tick: a battle, or the map going away.
static bool name_is_gear_screen_exact(const char* n) {
    return strstr(n, "CatSelector_")     != nullptr ||
           strstr(n, "InventoryButton_") != nullptr ||
           strstr(n, "EquippedButton_")  != nullptr ||
           strcmp(n, "Map_Backpack")     == 0;
}

static bool swap_director(uintptr_t* pd, uintptr_t* fd, uint32_t* pn, uint32_t* fn) {
    const void** slot = (const void**)((uintptr_t)GetModuleHandleW(nullptr) + kRva_MewDirectorPtr);
    const void*  md   = nullptr;
    if (!mem_read(slot, &md, sizeof(md)) || !md) return false;
    *pd = 0; *fd = 0; *pn = 0; *fn = 0;
    if (!mem_read((const uint8_t*)md + kDir_CatIdCount, pn, 4)) return false;
    if (!mem_read((const uint8_t*)md + kDir_CatIdData, pd, sizeof(*pd)) || !*pd) return false;
    if (!mem_read((const uint8_t*)md + kDir_CatFamiliars + 4, fn, 4)) return false;
    if (!mem_read((const uint8_t*)md + kDir_CatFamiliars + 8, fd, sizeof(*fd))) return false;
    return true;
}

// Read a run's id list as u32 (the low half, as read_party does), n entries.
static bool swap_read_ids(uintptr_t arr, uint32_t* out, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t raw[8] = {};
        if (!mem_read((const uint8_t*)arr + i * 8, raw, 8)) return false;
        memcpy(&out[i], raw, 4);
    }
    return true;
}

static bool swap_write_ids(uintptr_t arr, const uint32_t* in, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t v = in[i];
        if (!mem_write((uint8_t*)arr + i * 8, &v, 8)) return false;
    }
    return true;
}

// THE SHAPE CHECK MUST ACCEPT WHAT THE GAME ITSELF BUILDS (2026-09-29). The native
// post-battle promotion moves familiars into the party (Host3/Client2 -> party 4,
// familiars 1) WITHOUT touching the mirror, and a 1+1 run can leave the familiar
// list empty. The old check demanded mirror==party and familiars>0, refused that
// shape silently, and so the client could neither re-enter its local view on the
// map nor restore the shared roster at the next node ("node 4 NOT entered").
// replace_three rewrites and reserves all three vectors itself, so only an
// unreadable director or an impossible count is a reason to refuse.
static const char* g_swap_refused = nullptr;
static bool swap_lists(uintptr_t& pd, uintptr_t& fd, uintptr_t& md) {
    uint32_t pn = 0, fn = 0, mn = 0;
    uintptr_t dir = 0;
    const char* why = nullptr;
    if (!mem_read((const void*)addr_of_data(D_MewDirectorPtr), &dir, sizeof(dir)) || !dir)
        why = "no MewDirector";
    else if (!swap_director(&pd, &fd, &pn, &fn))
        why = "party vector unreadable";
    else if (pn == 0 || pn + fn > kSwapIds)
        why = "impossible party/familiar counts";
    else if (fn && !fd)
        why = "familiar vector has a count but no data";
    else if (!mem_read((const uint8_t*)dir + kDir_MirrorCount, &mn, 4) || mn > kSwapIds ||
             !mem_read((const uint8_t*)dir + kDir_MirrorData, &md, sizeof(md)))
        why = "mirror vector unreadable";
    else if (g_swap.captured && dir != g_swap.director)
        why = "MewDirector changed since capture";
    if (why) {
        if (why != g_swap_refused)
            log_line_lvl(LogLevel::Info, "ROSTER", "shared roster shape not usable yet: %s (party %u, familiars %u, mirror %u)",
                         why, pn, fn, mn);
        g_swap_refused = why;
        return false;
    }
    g_swap_refused = nullptr;
    return true;
}

static bool swap_set_lists(bool local,const char* why) {
    uintptr_t pd=0,fd=0,md=0;
    if(!swap_lists(pd,fd,md)) return false;
    const bool ok=replace_three(local?g_swap.local:g_swap.party_before,
        local?g_swap.ln:g_swap.pn,local?g_swap.others:g_swap.fam_before,local?g_swap.on:g_swap.fn);
    if(!ok && !g_swap_shape_invalid)log_line_lvl(LogLevel::Error,"ROSTER","%s: roster write failed",why);
    g_swap_shape_invalid=!ok;
    return ok;
}

static bool swap_release(const char* why) {
    if (!g_swap.captured) return true;
    if (!swap_set_lists(false, why)) return false;
    g_swap.active = false;
    g_swap_holds = false;
    return true;
}

static bool g_swap_blocked = false;
static bool g_swap_post = false;
static bool g_swap_list_seen = false;
static bool g_swap_battle_node = false;
static bool g_swap_node_entered = false;
static uint64_t g_swap_turn_serial = 0;
static uint64_t g_swap_run_turn_serial = 0;
static bool swap_capture();
static void swap_apply_now();

void party_swap_tick(void* button) {
    if (!g_setup_ready || !tune::kPartySwapWhileEditing || !net_active() || !button || net_peer_pos() == 0 ||
        g_swap_blocked) return;
    char name[64]{};
    const bool on_screen = mem_read_std_string((const uint8_t*)button + kBtn_Name_, name, sizeof(name)) &&
                           name_is_gear_screen_exact(name);
    if (on_screen || g_swap_post) swap_apply_now();
}

bool roster_party_swap_release() {
    if (!g_setup_ready || !tune::kPartySwapWhileEditing || !net_active() || net_peer_pos() == 0) return true;
    g_swap_blocked = true;
    g_swap_post = false;
    g_swap_list_seen = false;
    g_swap_battle_node = false;
    g_swap_node_entered = false;
    g_swap_turn_serial = g.turn_serial;
    if (!g_swap.captured) {
        uintptr_t pd = 0, fd = 0;
        uint32_t pn = 0, fn = 0;
        if (!swap_director(&pd, &fd, &pn, &fn) ||
            !swap_capture()) {
            log_line_lvl(LogLevel::Error, "ROSTER", "!! cannot capture the shared roster before node entry -- node must wait");
            return false;
        }
    }
    return swap_release("node entry: restoring shared roster");
}

static bool swap_capture() {
    if(g_swap.captured)return true;
    uintptr_t pd=0,fd=0,md=0;uint32_t pn=0,fn=0;
    if(!swap_lists(pd,fd,md)||!swap_director(&pd,&fd,&pn,&fn)||pn+fn>kSwapIds)return false;
    uint32_t party[16]{},fam[16]{};
    if(!swap_read_ids(pd,party,pn)||!swap_read_ids(fd,fam,fn))return false;
    const bool bootstrap=tune::kOwnershipSplit && net_peer_count()==2 && net_peer_pos()==1 &&
                          g.turn_serial==g_swap_run_turn_serial;
    // The shared layout is DERIVED from ownership, not copied from the live
    // lists: after a post-battle promotion the live party can hold another
    // peer's cat, and restoring that at node entry would hand it to the host.
    uint32_t local[16]{},other[16]{},ln=0,on=0;uint8_t owners[16]{};uint32_t all[16]{};
    uint32_t shared_party[16]{},shared_fam[16]{},spn=0,sfn=0;
    memcpy(all,party,pn*4);memcpy(all+pn,fam,fn*4);
    for(unsigned i=0;i<pn+fn;++i){
        if(!all[i]||all[i]==UINT32_MAX)return false;
        for(unsigned j=0;j<i;++j)if(all[i]==all[j])return false;
        uint8_t owner=kNoPeer;
        if(!lockstep_owner_pos(all[i],owner)) {
            const int encoded=session_cat_owner(all[i]);
            if(encoded>=0 && encoded<net_peer_count())owner=(uint8_t)encoded;
            else if(bootstrap)owner=i<pn?0:1;
            else return false;
        }
        if(owner>=net_peer_count())return false;
        owners[i]=owner;
        if(owner==0){if(spn>=4)return false;shared_party[spn++]=all[i];}else shared_fam[sfn++]=all[i];
        if(owner==net_peer_pos())local[ln++]=all[i];else other[on++]=all[i];
    }
    if(!ln||ln>4||!spn)return false;
    if(spn!=pn || !same_list(shared_party,spn,party,pn))
        log_line("ROSTER","shared roster captured with cats on the wrong side (party %u, familiars %u) -- "
                 "restoring by ownership: %u host cat(s) + %u other(s)",pn,fn,spn,sfn);
    memcpy(g_swap.party_before,shared_party,sizeof(shared_party));memcpy(g_swap.fam_before,shared_fam,sizeof(shared_fam));
    memcpy(g_swap.mirror_before,shared_party,sizeof(shared_party));
    memcpy(g_swap.local,local,sizeof(local));memcpy(g_swap.others,other,sizeof(other));
    g_swap.pn=spn;g_swap.fn=sfn;g_swap.ln=ln;g_swap.on=on;
    if(!mem_read((void*)addr_of_data(D_MewDirectorPtr),&g_swap.director,8))return false;
    for(unsigned i=0;i<pn+fn;++i)lockstep_note_owner(all[i],owners[i]);
    g_swap.captured=true;return true;
}

static void swap_apply_now() {
    // A failed shape read only suppresses the duplicate diagnostic.  It must
    // never become a latch: the scene teardown between a battle and the House
    // temporarily makes the three vectors unreadable, and the next valid tick
    // is exactly where the local roster has to be restored.
    if (!tune::kPartySwapWhileEditing || !net_active() || net_peer_pos() == 0) return;
    if (!swap_capture()) {
        if (!g_swap.waiting_reported) {
            g_swap.waiting_reported = true;
            log_line_lvl(LogLevel::Warn, "ROSTER", "!! local roster unavailable: need a readable shared roster and 1..4 cats owned by this peer -- NOT swapped");
        }
        return;
    }
    g_swap.waiting_reported = false;
    // Keep the per-run identities; an event's temporary list is not a new team selection.
    g_swap.active = true;
    g_swap_holds = true;
    swap_set_lists(true, "local roster: restoring this peer's selected cats");
}

void roster_party_swap_after_node(uint32_t type) {
    if (!g_setup_ready || !tune::kPartySwapWhileEditing || !net_active() || net_peer_pos() == 0) return;
    g_swap_node_entered = true;
    g_swap_battle_node = type >= 5 && type <= 8;
    const bool shared_event = type >= 9 && type <= 11 && !tune::kPerPlayerNodes;
    g_swap_blocked = g_swap_battle_node || shared_event;
    g_swap_post = !g_swap_blocked;
    g_swap_list_seen = false;
    if (g_swap_post) swap_apply_now();
}

void roster_party_swap_on_map() {
    // The host has no local view to restore; its party IS the shared one, so the
    // map tick is where a post-battle promotion is undone before anything reads it.
    if (g_setup_ready && net_active() && (net_peer_pos() == 0 || !tune::kPartySwapWhileEditing))
        roster_normalize_shared("map tick");
    if (!g_setup_ready || !tune::kPartySwapWhileEditing || !net_active() || net_peer_pos() == 0) return;
    // MapScreen::update is the first safe frame after a battle's TurnControl
    // is gone. Restore here immediately; waiting for a later node used to leave
    // the client displaying the host's shared party for one whole node.
    if (g_swap_blocked && !g_swap_node_entered) return;
    g_swap_blocked = false;
    g_swap_post = true;
    swap_apply_now();
}

void roster_party_swap_watch() {
    if (!g_setup_ready || !tune::kPartySwapWhileEditing || !net_active() || net_peer_pos() == 0 ||
        !g_swap_battle_node || !g_swap_node_entered || g_swap_post) return;
    // A stale holder before this node's first turn is not evidence that its battle started.
    if (g.turn_serial == g_swap_turn_serial) return;
    if (!lockstep_battle_list_gone()) {
        g_swap_list_seen = true;
        return;
    }
    // A fast debug clear can remove the battle list before the overlay gets a
    // frame in which to set g_swap_list_seen.  The turn serial already proves
    // this node has started, and lockstep_battle_list_gone() proves the live
    // roster is retired, so requiring the intermediate observation can leave
    // the client on the host party through the level-up screen.
    g_swap_list_seen = false;
    g_swap_blocked = false;
    g_swap_post = true;
    swap_apply_now();
}

void roster_party_swap_reset() {
    g_swap = {};
    g_swap_shape_invalid = false;
    g_swap_refused = nullptr;
    g_swap_run_turn_serial = g.turn_serial;
    g_swap_holds = false;
    g_swap_blocked = false;
    g_swap_post = false;
    g_swap_list_seen = false;
    g_swap_battle_node = false;
    g_swap_node_entered = false;
}

// POST-BATTLE PROMOTION, UNDONE BY OWNERSHIP (2026-09-29). Measured Host3/Client2: the
// node after the first battle was published as party [70000001 70000002 70000003
// 71000001] | familiars [71000002] -- the game had filled the party to four from the
// familiar list, i.e. handed a client cat to the host. The fixed 4+4 layout never
// showed it because a full party is never promoted into. Owner 0's cats belong in the
// party and everyone else's in the familiars; order is kept, so this is idempotent
// and writes nothing once the shape is right.
static bool g_norm_reported = false;
bool roster_normalize_shared(const char* why) {
    if (!g_setup_ready || !net_active()) return true;
    if (net_peer_pos() != 0 && g_swap.active) return true;   // the client's local view is the swap's business
    uintptr_t pd = 0, fd = 0; uint32_t pn = 0, fn = 0;
    if (!swap_director(&pd, &fd, &pn, &fn) || pn == 0 || pn + fn > kSwapIds || (fn && !fd)) return false;
    uint32_t all[kSwapIds]{};
    if (!swap_read_ids(pd, all, pn) || !swap_read_ids(fd, all + pn, fn)) return false;
    uint32_t party[4]{}, fam[kSwapIds]{}, npn = 0, nfn = 0;
    const char* refuse = nullptr;
    for (uint32_t i = 0; i < pn + fn && !refuse; ++i) {
        // A cat returning from leave_party_temporarily must not be listed twice.
        bool dup = false;
        for (uint32_t j = 0; j < i; ++j) dup = dup || all[j] == all[i];
        if (dup) continue;
        uint8_t owner = kNoPeer;
        if (!lockstep_owner_pos(all[i], owner)) {
            const int encoded = session_cat_owner(all[i]);
            if (encoded >= 0) owner = (uint8_t)encoded;
        }
        if (owner >= net_peer_count()) refuse = "a cat with no known owner";
        else if (owner == 0) { if (npn >= 4) refuse = "more than four host cats"; else party[npn++] = all[i]; }
        else fam[nfn++] = all[i];
    }
    if (!refuse && !npn) refuse = "no host cat left";
    if (refuse) {
        if (!g_norm_reported)
            log_line_lvl(LogLevel::Warn, "ROSTER", "!! %s: roster not normalized -- %s (party %u, familiars %u)",
                         why, refuse, pn, fn);
        g_norm_reported = true;
        return false;
    }
    if (npn == pn && same_list(party, npn, all, pn)) { g_norm_reported = false; return true; }
    if (!replace_three(party, npn, fam, nfn)) {
        if (!g_norm_reported)
            log_line_lvl(LogLevel::Error, "ROSTER", "!! %s: roster normalization write failed", why);
        g_norm_reported = true;
        return false;
    }
    g_norm_reported = false;
    log_line("ROSTER", "!! %s: native promotion undone -- party %u -> %u, familiars %u -> %u "
             "(every cat back on its owner's side)", why, pn, npn, fn, nfn);
    return true;
}

// THE HOST DECIDES WHICH CATS ARE IN THE RUN (2026-09-29). Measured Host3/Client2:
// 'CatHole' rolled its rare reward on the host -- leave_party_temporarily
// {fights_skipped 1} -- and cat 70000002 left the host's run. The client's swap had
// captured the shared layout once per run, so every node entry restored the frozen
// [70000001 70000002 70000003]; node 7 hashed 5 cats against the host's 4 and the
// battle halted at turn 1 (6 human cats vs 4: the absent cat's crow came along).
// The node snapshot is the host's run as that battle is built from it, so the
// client takes its membership from there and re-bases its local view on it.
static uint8_t shared_owner(uint32_t id) {
    uint8_t owner = kNoPeer;
    if (!lockstep_owner_pos(id, owner)) {
        const int encoded = session_cat_owner(id);
        if (encoded >= 0) owner = (uint8_t)encoded;
    }
    return owner;
}

bool roster_adopt_host_shared(const uint64_t* cats, uint32_t cn, const uint64_t* fam, uint32_t fn,
                              const char* why) {
    if (!g_setup_ready || !net_active() || net_peer_pos() == 0) return true;
    uint32_t party[4]{}, fams[kSwapIds]{}, all[kSwapIds]{}, npn = 0, nfn = 0;
    const char* refuse = nullptr;
    if (!cats || !cn || cn > kSwapIds || fn > kSwapIds || (fn && !fam)) refuse = "no usable lists";
    for (uint32_t i = 0; i < fn && !refuse; ++i) {
        if (!fam[i] || fam[i] >= UINT32_MAX) { refuse = "a bad familiar id"; break; }
        const uint8_t owner = shared_owner((uint32_t)fam[i]);
        if (owner == 0 || owner >= net_peer_count()) refuse = "a familiar not owned by a client";
        else fams[nfn++] = (uint32_t)fam[i];
    }
    for (uint32_t i = 0; i < cn && !refuse; ++i) {
        bool familiar = false;
        for (uint32_t j = 0; j < fn; ++j) familiar = familiar || fam[j] == cats[i];
        if (familiar) continue;
        if (!cats[i] || cats[i] >= UINT32_MAX) refuse = "a bad cat id";
        else if (shared_owner((uint32_t)cats[i]) != 0) refuse = "a party cat not owned by the host";
        else if (npn >= 4) refuse = "more than four host cats";
        else party[npn++] = (uint32_t)cats[i];
    }
    if (!refuse && !npn) refuse = "no host cat";
    if (!refuse && npn + nfn > kSwapIds) refuse = "too many cats";
    if (!refuse) {
        memcpy(all, party, npn * 4); memcpy(all + npn, fams, nfn * 4);
        for (uint32_t i = 0; i < npn + nfn && !refuse; ++i)
            for (uint32_t j = 0; j < i; ++j) if (all[i] == all[j]) { refuse = "a duplicated id"; break; }
    }
    uint32_t local[kSwapIds]{}, other[kSwapIds]{}, ln = 0, on = 0;
    for (uint32_t i = 0; i < npn + nfn && !refuse; ++i) {
        if (shared_owner(all[i]) == net_peer_pos()) local[ln++] = all[i]; else other[on++] = all[i];
    }
    if (!refuse && (!ln || ln > 4)) refuse = "this peer would keep no cat of its own";
    if (refuse) {
        log_line_lvl(LogLevel::Warn, "ROSTER", "!! %s: host roster NOT adopted -- %s (%u cat(s), %u familiar(s))",
                     why, refuse, cn, fn);
        return true;   // the node hash reports a real divergence; this is not the place to stop the run
    }
    uintptr_t pd = 0, fd = 0; uint32_t pn = 0, cfn = 0;
    uint32_t cur[kSwapIds]{};
    const bool readable = swap_director(&pd, &fd, &pn, &cfn) && pn + cfn <= kSwapIds && (!cfn || fd) &&
                          swap_read_ids(pd, cur, pn) && swap_read_ids(fd, cur + pn, cfn);
    const bool same = readable && pn == npn && cfn == nfn && same_list(cur, pn + cfn, all, npn + nfn);
    if (!same) {
        if (!replace_three(party, npn, fams, nfn)) {
            log_line_lvl(LogLevel::Error, "ROSTER", "!! %s: host roster write failed", why);
            return false;
        }
        char before[160]{}, after[160]{};
        int bo = 0, ao = 0;
        for (uint32_t i = 0; readable && i < pn + cfn && bo < (int)sizeof(before) - 12; ++i)
            bo += _snprintf_s(before + bo, sizeof(before) - bo, _TRUNCATE, "%s%x", i == pn ? " |" : " ", cur[i]);
        for (uint32_t i = 0; i < npn + nfn && ao < (int)sizeof(after) - 12; ++i)
            ao += _snprintf_s(after + ao, sizeof(after) - ao, _TRUNCATE, "%s%x", i == npn ? " |" : " ", all[i]);
        log_line_lvl(LogLevel::Warn, "ROSTER", "!! %s: adopted the host's roster --%s ->%s (party | familiars)",
                     why, readable ? before : " (unreadable)", after);
    }
    for (uint32_t i = 0; i < npn + nfn; ++i) lockstep_note_owner(all[i], shared_owner(all[i]));
    if (g_swap.captured) {
        memset(g_swap.party_before, 0, sizeof(g_swap.party_before));
        memset(g_swap.fam_before, 0, sizeof(g_swap.fam_before));
        memset(g_swap.mirror_before, 0, sizeof(g_swap.mirror_before));
        memset(g_swap.local, 0, sizeof(g_swap.local));
        memset(g_swap.others, 0, sizeof(g_swap.others));
        memcpy(g_swap.party_before, party, npn * 4); memcpy(g_swap.mirror_before, party, npn * 4);
        memcpy(g_swap.fam_before, fams, nfn * 4);
        memcpy(g_swap.local, local, ln * 4); memcpy(g_swap.others, other, on * 4);
        g_swap.pn = npn; g_swap.fn = nfn; g_swap.ln = ln; g_swap.on = on;
    }
    return true;
}

void roster_catselect_redirect(void* button) {
    if (tune::kPartySwapWhileEditing) { party_swap_tick(button); return; }

    if (!tune::kCatSelectRedirect || !button) return;

    char name[64] = {};
    if (mem_read_std_string((const uint8_t*)button + kBtn_Name_, name, sizeof(name)) &&
        name_is_gear_screen(name)) {
        g_red.seen_ms = GetTickCount();

        void* callable = nullptr;
        if (!mem_read((const uint8_t*)button + kBtn_Callback_, &callable, sizeof(callable)) || !callable)
            return;

        // ONE SCREEN, MANY BUTTONS -- and the key is the SCREEN, not the button.
        //
        // The first version compared the button's own callable and treated a different
        // one as "a different screen", so as soon as the detour moved from
        // CatSelector_Right to InventoryButton_head the redirect was restored and
        // re-installed: 8113 installs in one session, and the screen read the REAL
        // pointer on half the frames. Measured, not guessed -- the log alternated
        // "now answers with the whole run" and "is BACK on the run's party vector".
        //
        // The screen is recognised by the NAME of any of its buttons, so once it is
        // aimed, another button of the same screen must leave it alone.
        // EVERY BUTTON OF THE SCREEN HAS ITS OWN CLOSURE (measured: patching only the
        // first one left CatSelector_Right reading the real pointer, so the screen went on
        // showing four cats). Patch each distinct one exactly once.
        for (uint32_t i = 0; i < g_red.entry_n; ++i)
            if (g_red.entry[i].callable == callable) return;

        uintptr_t live = 0;
        if (!mem_read((const uint8_t*)callable + kCallable_List, &live, sizeof(live)) || !live)
            return;

        // A CLOSURE FIELD THAT DOES NOT HOLD A CAT LIST IS LEFT ALONE. The screen's OPENER
        // (Map_Backpack) is in the name list so the pointer can be aimed BEFORE the screen
        // is built -- it may read the list once, at open -- and that button's +0x10 may
        // hold something else entirely. The test is the vector triple itself: a plausible
        // count and a non-null array.
        {
            uint32_t  cnt = 0;
            uintptr_t arr = 0;
            mem_read((const uint8_t*)live + kDir_CatIdCount, &cnt, 4);
            mem_read((const uint8_t*)live + kDir_CatIdData,  &arr, sizeof(arr));
            if (!arr || cnt == 0 || cnt > 32) {
                if (!g_red.said_notlist) {
                    g_red.said_notlist = true;
                    log_line("ROSTER", "route A: a screen button's +0x10 does not hold a cat"
                                       " list (count %u, data 0x%llX) -- left alone",
                             cnt, (unsigned long long)arr);
                }
                return;
            }
        }

        const uint32_t n = collect_all_ids(g_red.ids, kMaxStubIds);
        if (n == 0) return;

        if (!g_red.active && !mem_read((const void*)live, g_red.stub, kStubBytes) && !g_red.said_short) {
            // A partial copy still carries the two fields below as long as they are inside
            // what did arrive; saying so beats a screen that quietly shows wrong numbers.
            g_red.said_short = true;
            log_line_lvl(LogLevel::Warn, "ROSTER",
                         "route A: the live object did not copy in full -- the stand-in is"
                         " only trustworthy for the fields that did");
        }

        const uint32_t  count     = n;
        const uintptr_t data      = (uintptr_t)g_red.ids;
        const uintptr_t stub_addr = (uintptr_t)g_red.stub;
        memcpy(g_red.stub + kDir_CatIdCount, &count, 4);
        memcpy(g_red.stub + kDir_CatIdData,  &data,  sizeof(data));

        if (!mem_write((uint8_t*)callable + kCallable_List, &stub_addr, sizeof(stub_addr))) {
            log_line_lvl(LogLevel::Error, "ROSTER",
                         "!! route A could not aim the gear screen at the stand-in -- the"
                         " screen still shows the party, nothing was changed");
            return;
        }
        if (g_red.entry_n >= kMaxRedirects) return;
        g_red.entry[g_red.entry_n].callable = callable;
        g_red.entry[g_red.entry_n].saved    = live;
        ++g_red.entry_n;
        g_red.active = true;

        char text[192] = {};
        int  off = 0;
        for (uint32_t i = 0; i < n && off < (int)sizeof(text) - 16; ++i)
            off += _snprintf_s(text + off, sizeof(text) - off, _TRUNCATE, "%s%llx",
                               i ? " " : "", (unsigned long long)g_red.ids[i]);
        log_line_lvl(LogLevel::Warn, "ROSTER",
                     "!! route A: the gear screen now answers with the WHOLE run's %u cat(s)"
                     " [%s] instead of the party's four -- so THIS peer can reach its own"
                     " familiars in the game's own screen. The run's party vector was NOT"
                     " touched and nothing was published; the pointer goes back when the"
                     " screen closes.", n, text);
        return;
    }

    // Not a gear-screen button on this frame. A screen is not redrawn every frame, so a
    // one-frame gap is not a close -- hence the grace period rather than an immediate
    // restore.
    if (g_red.active && (GetTickCount() - g_red.seen_ms) > kScreenGoneMs)
        redirect_restore();
}

bool roster_add_ids(const uint64_t* ids, uint32_t count, const char* why) {
    if (!ids || !count) return false;

    uint32_t have[kMaxPartyIds] = {};
    uint32_t n = 0;
    if (!read_party(have, n)) {
        log_line_lvl(LogLevel::Error, "ROSTER",
                     "!! %s: the run's cat list would not read -- nothing added", why);
        return false;
    }

    uint32_t out[kMaxPartyIds] = {};
    uint32_t on = 0;
    for (uint32_t i = 0; i < n && on < kMaxPartyIds; ++i) out[on++] = have[i];

    uint32_t added = 0;
    for (uint32_t i = 0; i < count && on < kMaxPartyIds; ++i) {
        // The run's list holds the low 32 bits of the id; kCatData_SaveId is the u64
        // the registry is keyed by. That split is measured, not assumed -- see the
        // stride-8 read in read_party.
        const uint32_t id = (uint32_t)ids[i];
        if (!id) continue;
        bool dup = false;
        for (uint32_t j = 0; j < on && !dup; ++j) dup = (out[j] == id);
        if (dup) continue;
        out[on++] = id;
        ++added;
    }

    if (!added) {
        log_line("ROSTER", "%s: all %u cat(s) were already in the run's list", why, count);
        return true;
    }

    char before[128] = {};
    if (!write_party(out, on, why, before, sizeof(before))) return false;

    log_line_lvl(LogLevel::Warn, "ROSTER",
                 "!! %s: added %u cat(s) to the run's party list -- %u cat(s) now. This"
                 " list is the layer that decides who fights, and both peers converge on"
                 " one ordered version of it through the normal merge.",
                 why, added, on);
    return true;
}

void roster_on_party(uint8_t from, const PartyMsg& m) {
    if (tune::kLocalSetup) return;
    char text[128] = {};
    ids_text(m.ids, m.count, text, sizeof(text));

    if (m.kind == 0) {
        memcpy(g.peer_ids, m.ids, sizeof(uint32_t) * m.count);
        g.peer_n = m.count;
        ++g.peers_seen;
        log_line("ROSTER", "<- PARTY from peer %u: [%s] -- %s", (unsigned)from, text,
                 (net_role() == NetRole::Host) ? "merging on the next map tick"
                                               : "peer 0 will send the agreed list");
        return;
    }

    // kind 1: the agreed list. The host does not need it (it computed it), and a
    // client writes exactly it -- the same ids, in the same order, which is what
    // makes the control split come out the same on both sides.
    if (net_role() == NetRole::Host) return;

    uint32_t mine[kMaxPartyIds] = {};
    uint32_t n = 0;
    read_party(mine, n);

    PartyMsg mine_msg{};
    mine_msg.kind  = 0;
    mine_msg.count = (uint8_t)n;
    for (uint32_t i = 0; i < n; ++i) mine_msg.ids[i] = mine[i];

    memcpy(g.agreed, m.ids, sizeof(uint32_t) * m.count);
    g.agreed_n = m.count;

    char before[128] = {};
    if (write_party(m.ids, m.count, "the agreed party from peer 0", before, sizeof(before)))
        log_line("ROSTER", "<- PARTY (agreed) from peer %u applied: [%s]", (unsigned)from, text);
    (void)mine_msg;
}

} // namespace mgmp
