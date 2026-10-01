// mgmp_listprobe.cpp -- see the header: why a watchpoint, what it watches, and
// what it cannot see.
//
// The one rule this file follows: IT OBSERVES. The handler logs, clears the trap
// and continues; it never writes to game memory, never changes a register the
// game owns, and never redirects control flow. Same rule as mgmp_crash, for the
// same reason -- an instrument that alters what it measures is not an instrument.

#include "mgmp_listprobe.h"

#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_tuning.h"

#include <windows.h>
#include <tlhelp32.h>   // CreateToolhelp32Snapshot/Thread32* -- the per-thread breakpoint arming
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// The chain mgmp_lockstep walks. Copied rather than shared because the two files
// mean different things by it: lockstep needs the roster, this needs the two
// addresses to put in a debug register.
constexpr uintptr_t kTC_Scene    = 0x18;
constexpr uintptr_t kScene_Sub   = 0x08;
constexpr uintptr_t kSub_Holder  = 0x20;   // the LIVE table: 16-byte entries holding a container*
constexpr uintptr_t kSub_Source  = 0x18;   // the SOURCE table, parallel and same indices:
                                           // {cap@+0, count@+4, array@+8}, filled in place
constexpr uintptr_t kHolder_List = 0x1F90;   // the slot index: 0x1F90/16 == 505, shared by both
constexpr uintptr_t kList_Cap    = 0x08;
constexpr uintptr_t kList_Count  = 0x0C;
constexpr uintptr_t kList_Data   = 0x10;

struct Slot {
    const char* what  = "";
    uintptr_t   addr  = 0;
    uint32_t    bytes = 0;   // 1, 2, 4 or 8 -- what the register watches
    bool        exec  = false;  // an EXECUTE breakpoint rather than a write watch
};

// THE APPEND, and why it is aimed at directly.
//
// rva 0x96B52B, located by observation (see the design notes, "The battle character
// list is rebuilt, not built"). It is an OUTLINED CHUNK: entered with its live
// state in rdi (the vector) and r14 (the candidate container) rather than in the
// ABI's argument registers, which is why it has no `call` OR `jmp` site anywhere
// in the image and why the stack was the only witness to its caller.
//
// An EXECUTE breakpoint on the entry solves both problems at once and is the only
// instrument here that does not depend on guessing: the trap is delivered BEFORE
// the instruction runs, and CONTEXT carries every register -- so rdi, r14 and
// [rsp] (the caller's return address) are read exactly rather than sampled.
//
// The prologue is checked before arming. A wrong address must produce a loud
// refusal, never a watchpoint on somebody else's code.
constexpr uint32_t kRva_Append    = 0x96B52B;
constexpr uint8_t  kAppendProlog[8] = { 0x4C,0x89,0x64,0x24,0x38, 0x4C,0x8B,0xE5 };

struct Hit {
    uintptr_t rip   = 0;
    uintptr_t watch = 0;   // WHICH address was being watched -- part of the key
    int       slot  = -1;
    uint32_t  n     = 0;
};

struct State {
    bool      on     = false;
    void*     tc     = nullptr;
    uintptr_t base   = 0;        // the exe, for filtering stack values to code
    uintptr_t holder = 0;        // the LIVE table base ([sub+0x20])
    uintptr_t src    = 0;        // the SOURCE table base ([sub+0x18]) -- the side that decides
    uintptr_t vec    = 0;        // the live container this frame (context for the hit line)
    uintptr_t vec_armed = 0;     // the live container slot 3 is aimed at
    bool      armed  = false;
    bool      append_armed = false;

    // MODE 2 (2026-09-23): the run's party vector, watched from the map and the House.
    // The data pointer moves when the game grows the array, so it is remembered to
    // notice that and re-aim -- those two id slots are only valid while it holds.
    uintptr_t party_dir   = 0;   // the MewDirector the slots are aimed at
    uintptr_t party_data  = 0;   // its +1472, the id array the two id slots watch
    uint32_t  party_count = 0;   // its +1468, watched directly (the stable one)
    // The SECOND id vector, 16 bytes after the party's own (2026-09-23). Remembered
    // only to notice it change; the offsets are a hypothesis, see the note by the
    // kSel_* constants.
    uintptr_t party_sel_data  = 0;
    uint32_t  party_sel_count = 0;
    Slot      slot[4] = {};
    Hit       hit[64] = {};
    uint32_t  hit_n  = 0;
    uint32_t  total  = 0;
};

State g;
PVOID g_veh = nullptr;
uint32_t g_append_hot = 0;   // calls that are not roster calls (see log_append_call)

// --- the self-test -----------------------------------------------------------
//
// A hardware watchpoint that did not take is invisible: SetThreadContext returns
// TRUE, no error is raised, and no trap ever arrives -- so "the game never wrote
// that field" and "the probe cannot see" produce the same log. The only way to
// tell them apart from inside the process is to fire one on purpose, at a
// variable of our own, and see whether it hurts.
volatile uint32_t g_self_word     = 0;
bool              g_self_test_pending = false;
bool              g_self_test_seen    = false;

// DR7 encodes the length, not the byte count: 1 -> 00, 2 -> 01, 8 -> 10, 4 -> 11.
uint32_t len_code(uint32_t bytes) {
    switch (bytes) {
        case 1:  return 0;
        case 2:  return 1;
        case 8:  return 2;
        default: return 3;      // 4
    }
}

// DR7 as g.slot asks for it. Split out so the value that was REQUESTED and the
// value that came back can be compared.
DWORD build_dr7() {
    DWORD dr7 = 0;
    for (int i = 0; i < 4; ++i) {
        if (!g.slot[i].addr) continue;
        dr7 |= (1u << (2 * i));                                        // L_i
        if (g.slot[i].exec) {
            // RW = 00 (execute) and LEN = 00 (one byte). An execute breakpoint
            // that asked for any other length would be silently ignored.
            continue;
        }
        dr7 |= (1u << (16 + 4 * i));                                   // RW_i = 01 (write)
        dr7 |= (len_code(g.slot[i].bytes) << (18 + 4 * i));            // LEN_i
    }
    return dr7;
}

// Programs DR0..DR3 and DR7 from g.slot, THEN READS THEM BACK.
//
// The read-back is the whole point. SetThreadContext returning TRUE does not
// mean the debug registers changed, and when they do not change there is no
// error, no exception and no trap -- the probe simply never fires again and the
// log reads exactly like "the game never writes this field". That is the worst
// possible failure mode for an instrument, so it is checked for instead of
// assumed: any requested bit missing from DR7, or any address that did not
// stick, is a failure the caller can print.
//
// Local-enable bits (L_i) rather than global: Windows saves and restores DR7 per
// thread, so L survives everything a thread does, and G is the bit that leaks
// across a debugger's session.
bool program_slots(uintptr_t* rb_dr0, DWORD* rb_dr7) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(GetCurrentThread(), &c)) return false;

    c.Dr0 = g.slot[0].addr;
    c.Dr1 = g.slot[1].addr;
    c.Dr2 = g.slot[2].addr;
    c.Dr3 = g.slot[3].addr;
    c.Dr7 = build_dr7();
    c.Dr6 = 0;

    if (!SetThreadContext(GetCurrentThread(), &c)) return false;

    CONTEXT rb{};
    rb.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(GetCurrentThread(), &rb)) return false;
    if (rb_dr0) *rb_dr0 = (uintptr_t)rb.Dr0;
    if (rb_dr7) *rb_dr7 = (DWORD)rb.Dr7;

    // Every bit we asked for must be present. Extra bits set by something else
    // are not our business, so this is a subset test rather than equality.
    if (((DWORD)rb.Dr7 & c.Dr7) != c.Dr7) return false;
    return rb.Dr0 == c.Dr0 && rb.Dr1 == c.Dr1 && rb.Dr2 == c.Dr2 && rb.Dr3 == c.Dr3;
}

// Consecutive arming failures. One can be transient; a run of them means the
// mechanism is not working and saying so beats logging it every frame.
uint32_t g_arm_failures = 0;

// WHAT THE THREAD'S DEBUG REGISTERS ACTUALLY HOLD, ASKED EVERY SO OFTEN.
//
// The probe's worst failure is the quiet one: the registers are gone, no trap
// ever arrives again, and the log reads exactly like "the game does not write
// that field". That happened -- an hour of play produced no hits on a vector
// that demonstrably held 32 cats, and a read of the thread's Dr0..Dr3/D7 showed
// slot 0 armed and slots 1..3 zeroed, with the probe still believing it had all
// four (see the measurement note on the in-trap re-arm below).
//
// ARMED IS A CLAIM, SO IT IS CHECKED. One GetThreadContext every half second
// costs nothing next to a frame, and it turns "went deaf" into a log line and a
// re-arm instead of a mystery.
bool slots_live() {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(GetCurrentThread(), &c)) return false;
    if (c.Dr0 != g.slot[0].addr || c.Dr1 != g.slot[1].addr ||
        c.Dr2 != g.slot[2].addr || c.Dr3 != g.slot[3].addr) return false;
    return ((DWORD)c.Dr7 & build_dr7()) == build_dr7();
}

// Re-programs the registers if they were taken away -- from the TICK, i.e. from
// an ordinary context, because that is the only place where programming them
// works at all.
void verify_and_heal() {
    if (!g.armed || slots_live()) return;

    static uint32_t relost = 0;
    ++relost;
    const bool ok = program_slots(nullptr, nullptr);
    if (relost == 1 || (relost % 60) == 0)
        log_line_lvl(LogLevel::Warn, "PROBE",
                     "!! the watchpoints were not in this thread's debug registers (%u time(s) so "
                     "far) -- re-armed %s. Slot 0 is 0x%llX, slot 1 0x%llX, slot 2 0x%llX, slot 3 "
                     "0x%llX; a VEH cannot do this for itself (see the measurement note below), so "
                     "anything written between the loss and this line was NOT observed.",
                     relost, ok ? "OK" : "and it DID NOT TAKE",
                     (unsigned long long)g.slot[0].addr, (unsigned long long)g.slot[1].addr,
                     (unsigned long long)g.slot[2].addr, (unsigned long long)g.slot[3].addr);
    if (!ok && ++g_arm_failures >= 3) {
        log_line_lvl(LogLevel::Warn, "PROBE", "!! giving up: the roster probe is OFF");
        g.on = false;
    }
}

void report_arming(const char* what) {
    uintptr_t rb_dr0 = 0;
    DWORD     rb_dr7 = 0;
    const bool ok = program_slots(&rb_dr0, &rb_dr7);

    log_line("PROBE", "armed %s (holder=0x%llX vec=0x%llX src=0x%llX)",
             what, (unsigned long long)g.holder, (unsigned long long)g.vec,
             (unsigned long long)g.src);
    for (int i = 0; i < 4; ++i)
        if (g.slot[i].addr)
            log_line("PROBE", "   slot[%d] %-13s @0x%llX  %u byte(s)%s",
                     i, g.slot[i].what, (unsigned long long)g.slot[i].addr,
                     g.slot[i].bytes, g.slot[i].exec ? "  EXECUTE breakpoint" : "  write watch");

    if (ok) { g_arm_failures = 0; return; }

    ++g_arm_failures;
    log_line_lvl(LogLevel::Warn, "PROBE",
                 "!! the watchpoints did NOT take (try %u): DR0 read back 0x%llX (wanted 0x%llX), "
                 "DR7 0x%X (wanted 0x%X), last error %lu -- anything the probe reports after this "
                 "is NOT evidence of absence",
                 g_arm_failures, (unsigned long long)rb_dr0, (unsigned long long)g.slot[0].addr,
                 rb_dr7, build_dr7(), GetLastError());
    if (g_arm_failures >= 3) {
        log_line_lvl(LogLevel::Warn, "PROBE", "!! giving up: the roster probe is OFF");
        g.on = false;
    }
}

// Defined below, next to the failure note that explains what it replaced.
void log_callers(const CONTEXT* ctx);

// A write trap is delivered AFTER the instruction, so Rip is the instruction
// after the writer. Dumping both sides is what lets the writer be disassembled
// offline instead of guessed at from the trap address.
void log_hit(const CONTEXT* c, int slot) {
    const uintptr_t rip = (uintptr_t)c->Rip;

    // WHAT USED TO BE HERE, AND THE MEASUREMENT THAT KILLED IT.
    //
    // The handler used to re-arm slots 1..3 the instant slot 0 fired, on the
    // reasoning that the copy which fills a freshly installed container starts a
    // few instructions later in the same call, so waiting for the next frame is
    // a whole frame too late. `program_slots` even read the registers back and
    // found them as asked, and the log said "re-armed INSIDE the trap".
    //
    // IT WAS NOT RE-ARMED. Windows discards a DEBUG_REGISTERS change made by an
    // exception handler for the thread that handler is running on: the kernel
    // reloads DR7 from the state it saved when the #DB was taken. Inside the
    // handler the pending context reads back as written, so the read-back cannot
    // see the difference -- the one failure mode this file's whole design is
    // against, produced by the probe itself.
    //
    // Measured, not inferred (_buildcheck/m0_dr_intrap.cpp, its own process):
    //   arm slot 3 on a global from a normal context -> write it -> #DB arrives;
    //   in that handler, arm slot 2 on a second global; read back: slot 2 armed;
    //   after the handler returns, from a normal context: dr2 == 0, dr7 back to
    //   its pre-trap value; write the slot-2 address: NO trap.
    //
    // So no race can be closed from inside a handler, and silence after slot 0
    // was never evidence about the fill. The arming all happens in the tick now,
    // which is an ordinary context, and slots_live() makes the loss visible if
    // anything takes the registers away again.
    //
    // The key is (rip, slot, WATCHED ADDRESS), and the address is the part that
    // was missing: with (rip, slot) alone, a write site that spent its five-log
    // budget in one battle printed nothing at all in the next -- which is how a
    // whole battle's roster fill went missing from the log while the probe was,
    // in fact, watching it happen.
    const uintptr_t watch = g.slot[slot].addr;
    Hit* h = nullptr;
    for (uint32_t i = 0; i < g.hit_n; ++i)
        if (g.hit[i].rip == rip && g.hit[i].slot == slot && g.hit[i].watch == watch) {
            h = &g.hit[i];
            break;
        }
    if (!h && g.hit_n < 64) {
        h = &g.hit[g.hit_n++];
        h->rip = rip; h->slot = slot; h->watch = watch; h->n = 0;
    }
    ++g.total;
    if (h) {
        ++h->n;
        // A fill is dozens of writes from one site. Five verbatim, then a
        // heartbeat, so the log stays readable and the count is still honest.
        if (h->n > 5 && (h->n % 200) != 0) return;
    }

    uint32_t  cap = 0, cnt = 0;
    uintptr_t data = 0, vecslot = 0;
    if (g.vec) {
        mem_read((const uint8_t*)g.vec + kList_Cap,   &cap, 4);
        mem_read((const uint8_t*)g.vec + kList_Count, &cnt, 4);
        mem_read((const uint8_t*)g.vec + kList_Data,  &data, sizeof(data));
    }
    mem_read((const uint8_t*)g.holder + kHolder_List, &vecslot, sizeof(vecslot));

    char hex[3 * 33] = {};
    mem_hexdump(hex, sizeof(hex), (const uint8_t*)(rip - 16), 32);

    log_line("PROBE", "hit %u [%s] rip=0x%llX watch=0x%llX  count=%u cap=%u data=0x%llX "
                      "holder=0x%llX vec=0x%llX holder[+1F90]=0x%llX tid=%lu",
             g.total, g.slot[slot].what, (unsigned long long)rip,
             (unsigned long long)watch, cnt, cap,
             (unsigned long long)data, (unsigned long long)g.holder,
             (unsigned long long)g.vec, (unsigned long long)vecslot,
             (unsigned long)GetCurrentThreadId());
    log_line("PROBE", "     bytes @rip-16: %s", hex);

    // The source entry, spelled out at the moment it is written. The hit line
    // above reports the LIVE container's cap/count/data -- useful context, but it
    // is the copy's output. This is the input: the triple the copy reads, and the
    // one whose writer is still unknown. Printed here so a log can be read on its
    // own, without a second pass through a live process.
    if (g.src) {
        uint32_t  scap = 0, scnt = 0;
        uintptr_t sarr = 0;
        const uintptr_t s = g.src + kHolder_List;
        if (mem_read((const uint8_t*)s + 0, &scap, 4) &&
            mem_read((const uint8_t*)s + 4, &scnt, 4) &&
            mem_read((const uint8_t*)s + 8, &sarr, sizeof(sarr)))
            log_line("PROBE", "     source entry: cap=%u count=%u array=0x%llX   (the side that "
                              "decides who is in the battle)", scap, scnt,
                     (unsigned long long)sarr);
    }

    // THE CALLER CHAIN, UNWOUND PROPERLY.
    //
    // This used to be a scan of the raw stack for "words that look like code",
    // which cannot tell a return address from a saved pointer: it reported the
    // frame whose value happened to match a pattern, never the frame that called.
    // The exe is an ordinary x64 image with .pdata unwind info, so the real chain
    // is one RtlVirtualUnwind loop away -- and it is the difference between "some
    // code near here" and "this function, at this offset, called it".
    log_callers(c);
}

// Walks the stack out of the trapped context using the image's own unwind data.
//
// RtlLookupFunctionEntry/RtlVirtualUnwind are taken from ntdll by name rather
// than linked, because the mod does not otherwise import ntdll and one more
// dependency is not worth it for a debug-only path.
//
// Callers are printed as RVAs: two processes of the same build have different
// bases, and an RVA is the only form in which two logs can be compared or looked
// up in a disassembler.
void log_callers(const CONTEXT* ctx) {
    typedef PRUNTIME_FUNCTION (WINAPI* FnLookup)(DWORD64, PDWORD64, PVOID);
    typedef PEXCEPTION_ROUTINE (WINAPI* FnUnwind)(ULONG, DWORD64, DWORD64, PRUNTIME_FUNCTION,
                                                  PCONTEXT, PVOID*, PDWORD64,
                                                  PKNONVOLATILE_CONTEXT_POINTERS);
    static FnLookup lookup = (FnLookup)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                      "RtlLookupFunctionEntry");
    static FnUnwind unwind = (FnUnwind)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                      "RtlVirtualUnwind");
    if (!lookup || !unwind) {
        log_line("PROBE", "     callers: <ntdll unwinder unavailable>");
        return;
    }

    CONTEXT c = *ctx;             // the walk mutates it
    char line[512] = {};
    int  off = 0, shown = 0, not_exe = 0;
    for (int i = 0; i < 14 && shown < 8; ++i) {
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION rf = lookup(c.Rip, &image_base, nullptr);
        if (!rf) break;                                   // leaf: no unwind info
        PVOID    handler     = nullptr;
        DWORD64  establisher = 0;
        KNONVOLATILE_CONTEXT_POINTERS nv{};
        unwind(UNW_FLAG_NHANDLER, image_base, c.Rip, rf, &c, &handler, &establisher, &nv);
        if (!c.Rip) break;

        if (c.Rip >= g.base && c.Rip < g.base + 0x2000000ull) {
            off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " 0x%llX",
                               (unsigned long long)(c.Rip - g.base));
            ++shown;
        } else {
            // A frame outside the game is worth one marker, not a dump: it is
            // either a system call the game is inside of or the end of our chain.
            if (not_exe++ == 0)
                off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " [other]");
        }
        if (off >= (int)sizeof(line) - 24) break;
    }

    if (shown) log_line("PROBE", "     callers(rva):%s", line);
    else       log_line("PROBE", "     callers: <none in the game image>");
}

// THE EXECUTE-BREAKPOINT READER THAT USED TO LIVE HERE IS GONE ON PURPOSE.
//
// It pointed a DRx *execute* breakpoint at the append chunk's entry to read rdi
// and r14, which are not argument registers. Two facts killed it: the chunk is a
// SHARED OUTLINED BODY that a hot unrelated path also runs (~27,000 times a
// second on the main menu alone, measured), and an execute breakpoint is a FAULT
// -- resuming from a VEH assumes the resume flag suppresses the re-trigger, and
// pointing that at a hot address hung the game. The write watchpoints below never
// did that in three rounds of use, so they are what stayed.

LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;

    // Dr6 is 64-bit in the AMD64 CONTEXT, and its upper bits are RESERVED-1, so
    // the flag test is written against the 64-bit value rather than truncated:
    // a 32-bit `!= 0` would be true for every trap the machine ever takes.
    const DWORD64 dr6 = ep->ContextRecord->Dr6;
    int slot = -1;
    for (int i = 0; i < 4; ++i)
        if (dr6 & (1ull << i)) { slot = i; break; }
    if (slot < 0) return EXCEPTION_CONTINUE_SEARCH;   // a trap that is not ours

    // MODE 3: READ THE ARGUMENT REGISTERS OF A BUTTON CALLBACK (2026-09-23, route A).
    //
    // The one place in this module that reads registers the ABI does not hand over in
    // the trap's own memory. It is safe HERE and was not safe on the address the old
    // reader used: that one was a shared outlined chunk taken ~27,000 times a second by
    // an unrelated hot loop, and trapping it hung the game. This one is a button
    // callback -- it runs when somebody clicks an arrow -- and it is capped at 8 hits
    // below, which the log states, so a surprise cannot turn into a flood.
    if (tune::kCatSelectorProbe && g.slot[slot].exec) {
        static uint32_t shown = 0;
        if (++shown <= 8) {
            const CONTEXT* c   = ep->ContextRecord;
            const uintptr_t obj = (uintptr_t)c->Rcx;
            uintptr_t p8 = 0, p10 = 0, d8 = 0, d10 = 0;
            uint32_t  n8 = 0, n10 = 0;
            mem_read((const void*)(obj + 8),    &p8,  sizeof(p8));
            mem_read((const void*)(obj + 0x10), &p10, sizeof(p10));
            if (p8) {
                mem_read((const uint8_t*)p8 + 0x5BC, &n8,  4);
                mem_read((const uint8_t*)p8 + 0x5C0, &d8,  sizeof(d8));
            }
            if (p10) {
                mem_read((const uint8_t*)p10 + 0x5BC, &n10, 4);
                mem_read((const uint8_t*)p10 + 0x5C0, &d10, sizeof(d10));
            }
            log_line_lvl(LogLevel::Warn, "PROBE",
                         "!! SELECTOR #%u (cap 8) rcx=0x%llX  [rcx+8]=0x%llX"
                         " {+5BC count %u, +5C0 data 0x%llX}   [rcx+10]=0x%llX"
                         " {+5BC count %u, +5C0 data 0x%llX}   rip=0x%llX",
                         shown, (unsigned long long)obj, (unsigned long long)p8, n8,
                         (unsigned long long)d8, (unsigned long long)p10, n10,
                         (unsigned long long)d10, (unsigned long long)c->Rip);
        }
    }

    // The self-test comes FIRST, before any state check, because it fires during
    // init -- when the probe is not armed and has no battle. A single-step trap
    // that nobody consumes is not a missing log line; it is a crash.
    if (g_self_test_pending) {
        g_self_test_seen = true;
        ep->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // A trap on one of our four slots is CONSUMED even when the probe is off or
    // unarmed: leaving it for the game means an unhandled single-step. (The cost
    // of this rule is that a debugger setting DR0-DR3 on this thread would have
    // its watchpoints eaten by us -- there is no debugger here, and a crash is
    // the worse of the two.)
    if (g.on && g.armed) log_hit(ep->ContextRecord, slot);

    // AN EXECUTE BREAKPOINT NEEDS THE RESUME FLAG; A WRITE WATCHPOINT DOES NOT
    // (2026-09-23, measured the hard way -- 6,766,400 traps, a 12 MB log and a hung
    // game, from one click on a button).
    //
    // A data breakpoint traps AFTER the store has executed, which is why clearing Dr6
    // below is enough for one: there is nothing left to re-trigger (the hit lines show
    // it -- `rip` is the instruction AFTER the writing one). An execute breakpoint traps
    // BEFORE the instruction, so without RF it faults again immediately and forever. RF
    // suppresses breakpoints for exactly one instruction, which is the one to resume.
    if (g.slot[slot].exec) ep->ContextRecord->EFlags |= 0x10000;   // RF (bit 16)

    // Clear the status bit and keep DR7: the breakpoint stays armed for the next
    // write. Without this the same condition retriggers forever.
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Prove the mechanism on our own memory before it is trusted with anyone's.
//
// The interesting failure is not "the registers could not be set" -- that one is
// caught by the read-back in program_slots. It is "the registers are set and the
// CPU still does not trap": a watchpoint whose length and alignment disagree
// silently watches nothing, and a hypervisor or an unusual thread state can eat
// the #DB. Either way the probe would report nothing forever and look correct.
void run_self_test() {
    const Slot saved = g.slot[3];
    g.slot[3] = { "self", (uintptr_t)&g_self_word, 4 };
    g_self_test_seen = false;

    uintptr_t rb0 = 0;
    DWORD     rb7 = 0;
    if (!program_slots(&rb0, &rb7)) {
        log_line_lvl(LogLevel::Warn, "PROBE",
                     "!! selftest: the watchpoint would not arm (DR7 read back 0x%X, wanted 0x%X, "
                     "DR0 0x%llX) -- the roster probe is OFF",
                     rb7, build_dr7(), (unsigned long long)rb0);
        g.slot[3] = saved;
        g.on = false;
        return;
    }

    g_self_test_pending = true;
    g_self_word += 1;                  // this store must trap, here, synchronously
    g_self_test_pending = false;

    g.slot[3] = saved;
    program_slots(nullptr, nullptr);

    if (g_self_test_seen) {
        log_line("PROBE", "selftest: a watchpoint on the probe's OWN memory trapped -- hardware "
                          "watchpoints work on this thread. From here on, a silent log means the "
                          "game did not write, not that the probe cannot see");
    } else {
        log_line_lvl(LogLevel::Warn, "PROBE",
                     "!! selftest FAILED: the debug registers were accepted and no #DB arrived. "
                     "Nothing this probe reports can be trusted, and in particular NO HITS IS NOT "
                     "EVIDENCE that the game never writes the list. The roster probe is OFF.");
        g.on = false;
    }
}

} // namespace

// --- naming the screens, for the run-setup work ------------------------------
//
// TEMPORARY, and the cheapest instrument in this file. The setup flow a run
// begins with -- party, equipment, chapter -- is the one part of starting a game
// the mod does not touch, and the question about it is not "how would I hook
// these screens" but "which button COMMITS each one".
//
// Button+504 is a std::string holding the button's own name. Two independent
// readings pin it: MainMenu::init assigns "MainMenu_Button_Play" there, and
// Button::Click reads the SIZE of that same string (504 + 16, where an MSVC
// std::string keeps its length) to build the "..._Click" sound event. And
// h_ButtonUpdate already fires for EVERY button in the game -- it is how the
// client's Play and Quit To Menu presses are made -- so printing each name once
// walks the whole flow out of the log.
//
// Deduped because a button updates every frame, capped because this runs on that
// path, and it writes nothing: same rule as the rest of this file.
void listprobe_on_button(void* button) {
    if (!tune::kLogButtons || !button) return;

    constexpr uintptr_t kBtn_Name = 504;   // mgmp_addresses.h kBtn_Name, local for
                                           // the same reason this file's chain is
    char name[64];
    if (!mem_read_std_string((const uint8_t*)button + kBtn_Name, name, sizeof(name)))
        return;
    if (!name[0]) return;

    static char     seen[256][64];
    static uint32_t seen_n = 0;
    for (uint32_t i = 0; i < seen_n; ++i)
        if (strcmp(seen[i], name) == 0) return;
    if (seen_n >= 256) return;
    strcpy_s(seen[seen_n], name);
    ++seen_n;

    // THE NAME IDENTIFIES THE SCREEN; THE CALLBACK IS WHAT THE PRESS DOES.
    //
    // For a screen the mod has to drive -- the run's setup flow, where the host
    // chooses the party, the gear and the chapter -- "which button commits this"
    // is the only question, and it is a memory read rather than a search: the
    // project recorded Button::Click's body in mgmp_addresses.h, and the click is
    //
    //     v8 = *(Button + 240);  if (v8) (*(*(_QWORD*)v8 + 16))(v8);
    //
    // i.e. +240 holds a pointer to a polymorphic callable whose slot 2 is the
    // _Do_call of the std::function that MenuPanel::register_button stored. That
    // slot is the function that runs, and slot 0 of the same vtable leads to the
    // lambda's own RTTI name -- so one line names the commit AND its class.
    //
    // The name table above is why this is worth doing: these strings live in the
    // resource symbol pool (neighbours of HouseStatusUI and object_cattree1), not
    // as literals any code references, so the literal is a dead end and the
    // callback is the way in.
    //
    // SELF-CHECKING, which is why it is safe to trust: the main menu's Play
    // button must come back as rva 0x1BEBE0, the lambda this project pinned in
    // August. A wrong offset here produces a wild pointer instead, and the bound
    // below is what decides between the two.
    constexpr uintptr_t kBtn_Callback = 240;
    const uintptr_t     base = (uintptr_t)GetModuleHandleW(nullptr);
    void* cb = nullptr;
    if (!mem_read((const uint8_t*)button + kBtn_Callback, &cb, sizeof(cb)) || !cb) {
        log_line("BUTTON", "%-34s (no callback on this button)", name);
        return;
    }

    void* vt = nullptr;
    void* target = nullptr;
    if (!mem_read((const uint8_t*)cb, &vt, sizeof(vt)) || !vt ||
        !mem_read((const uint8_t*)vt + 16, &target, sizeof(target)) || !target ||
        (uintptr_t)vt < base || (uintptr_t)vt > base + 0x2000000 ||
        (uintptr_t)target < base || (uintptr_t)target > base + 0x2000000) {
        log_line("BUTTON", "%-34s callback 0x%llX does not read as a std::function -- "
                           "reporting the pointer alone", name,
                 (unsigned long long)(uintptr_t)cb);
        return;
    }

    log_line("BUTTON", "%-34s cb=0x%llX  target=0x%llX (rva 0x%llX)", name,
             (unsigned long long)(uintptr_t)cb, (unsigned long long)(uintptr_t)target,
             (unsigned long long)((uintptr_t)target - base));
}

void listprobe_init() {
    // EITHER QUESTION ARMS THE INSTRUMENT (2026-09-23). This gate used to be
    // tune::kRosterProbe alone, which made the party-vector mode added today
    // unreachable: listprobe_tick returns immediately while g.on is false, and g.on is
    // only ever set here. Measured by its absence -- a session standing in the House
    // produced no arming line at all, which is exactly what "the gate is upstream of
    // the thing you are testing" looks like.
    if (!tune::kRosterProbe && !tune::kPartyProbe && !tune::kCatSelectorProbe) return;

    g.base = (uintptr_t)GetModuleHandleW(nullptr);
    g.on   = true;
    g_veh  = AddVectoredExceptionHandler(1, veh);
    if (!g_veh) {
        g.on = false;
        log_line_lvl(LogLevel::Warn, "PROBE",
                     "!! AddVectoredExceptionHandler failed (%lu) -- no roster probe",
                     GetLastError());
        return;
    }

    log_line_lvl(LogLevel::Warn, "PROBE",
                 "!! PROBE on -- %s. Hardware write watchpoints; it observes and never writes, "
                 "and it is an RE instrument rather than a feature (see mgmp_listprobe.h)",
                 tune::kPartyProbe
                     ? "MODE 2: the RUN's party vector (MewDirector+1468 count, +1472 data, "
                       "and the first two ids)"
                     : "MODE 1: the battle character list");
    if (tune::kProbeAppend)
        log_line("PROBE", "aimed at the APPEND CHUNK as an EXECUTE breakpoint -- no TurnControl "
                          "needed, so the first battle's fill is caught too. Stage 2 watches the "
                          "candidate container it is handed, to find what fills THAT");
    else
        log_line("PROBE", "waiting for a TurnControl -- the chain is walked every frame, "
                          "and the vector is re-armed whenever the battle replaces it");
    run_self_test();
    if (!g.on) return;
}

void listprobe_set_turn_control(void* turn_control) {
    if (!g.on || !turn_control) return;
    if (turn_control == g.tc) return;
    g.tc = turn_control;
    log_line("PROBE", "turn control 0x%llX -- watching its cat list from here on",
             (unsigned long long)(uintptr_t)turn_control);
}

// (The append-entry arming that used to be here is gone -- see the note at the
// end of this file's helper section. What is worth keeping from it is the
// prologue check: the address came from a RUNNING PROCESS, not from a symbol
// table, so on any other build it is a guess, and a breakpoint on a guess lands
// on somebody else's code and reports its traffic as the roster's. Read the bytes
// back and refuse loudly.)
// ---------------------------------------------------------------------------
// MODE 2: WHO WRITES THE RUN'S PARTY VECTOR (2026-09-23)
//
// WHY A SECOND MODE RATHER THAN RE-AIMING THE FIRST. The battle list above is reached
// from a TurnControl, which exists only while a battle does. The event this question is
// about happens with the player standing in the HOUSE and no battle anywhere: they walk
// out of the door (Depart_Sign), and the run's party is set. There is nothing for the
// old chain to hang off. The address to watch is a plain data symbol instead:
//
//     MewDirector + 1468  (u32)   the party's COUNT
//     MewDirector + 1472  (ptr)   the party's id array
//
// which is the layer this file's own notes and the design notes both end up pointing at
// ("The layer that actually decides is the run's cat id vector at MewDirector+1468").
// The battle-side tables are DERIVED from this one, which is why watching them answered
// a different question.
//
// The slots, and what each is for:
//
//   0  +1468 count     a STABLE address, and changing the party's SIZE is the event
//                      being looked for -- the RIP that traps here names the function
//                      that sets the party on departure, which is the answer.
//   1  +1472 pointer   fires when the game reallocates the array (a grown party gets a
//                      fresh buffer), i.e. the other half of a fill.
//   2  data[0]         the appends themselves, while the buffer lasts.
//   3  data[1]
//
// Slots 2 and 3 go stale when the buffer moves; the tick re-reads the pointer every
// frame and re-aims, the same way the battle mode re-aims when its container is
// replaced. Nothing here writes game memory, and nothing redirects control flow.
static constexpr uintptr_t kDir_Party       = 1464;   // {cap, count+4, data+8} -- measured
static constexpr uintptr_t kDir_CatIdCount_ = 1468;   // the party's count (0x5BC)
static constexpr uintptr_t kDir_CatIdData_  = 1472;   // the party's id array (0x5C0)

// THE SECOND ID VECTOR, FOUND IN THE DEPARTURE CODE (2026-09-23).
//
// The static dump of the function that owns the party's count write (RVA 0x3B1480,
// reached from 0xAE6EA -> 0xAE145 -> the Depart_Sign handler 0x1F8ED3) shows it doing
// a vector-assign TWICE, sixteen bytes apart: once on [rsi+0x5B8] (the party, +1464)
// and once on [rsi+0x5C8] (+1480), each time free-then-memcpy through the same 8-byte
// element grow helper (0x48FB0) and 0xDAB860. That is the shape of "copy the chosen
// cats into the run", which is why the second vector is worth watching: if the House's
// selection lives there, the game-native way to add cats to a run is to put them in
// the SELECTION and let departure do the copy -- instead of writing the party directly,
// which is what crashes on quit above five cats (FEASIBILITY §10.3).
//
// THE OFFSETS BELOW ARE A HYPOTHESIS: the layout is assumed to match its neighbour's
// {cap, count, data}. That assumption is why party_tick prints the raw 32 bytes at
// +1464 on every change -- one look settles it either way.
static constexpr uintptr_t kSel_Cap_   = 1480;
static constexpr uintptr_t kSel_Count_ = 1484;
static constexpr uintptr_t kSel_Data_  = 1488;

// Hardcoded the way this file's other addresses are (see kRva_Append): a debug
// instrument reads what the running game has. This is kRva_MewDirectorPtr from
// mgmp_addresses.h -- the data symbol mgmp_catsync and mgmp_roster call D_MewDirectorPtr.
static constexpr uintptr_t kMewDirectorPtrRva = 0x013DAC30;

static void party_tick() {
    const void* md = nullptr;
    if (!mem_read((const void*)(g.base + kMewDirectorPtrRva), &md, sizeof(md)) || !md) return;

    const uintptr_t dir = (uintptr_t)md;
    uint32_t  cap = 0, count = 0, sel_cap = 0, sel_count = 0;
    uintptr_t data = 0, sel_data = 0;
    if (!mem_read((const uint8_t*)dir + kDir_Party,      &cap,       4)) return;
    if (!mem_read((const uint8_t*)dir + kDir_CatIdCount_, &count,    4)) return;
    if (!mem_read((const uint8_t*)dir + kDir_CatIdData_, &data,     sizeof(data))) return;
    if (!mem_read((const uint8_t*)dir + kSel_Cap_,       &sel_cap,   4)) return;
    if (!mem_read((const uint8_t*)dir + kSel_Count_,     &sel_count, 4)) return;
    if (!mem_read((const uint8_t*)dir + kSel_Data_,      &sel_data, sizeof(sel_data))) return;

    const bool changed = (dir != g.party_dir) || (data != g.party_data) ||
                         (count != g.party_count) || (sel_data != g.party_sel_data) ||
                         (sel_count != g.party_sel_count);
    if (!changed) {
        verify_and_heal();
        return;
    }

    g.party_dir       = dir;
    g.party_data      = data;
    g.party_count     = count;
    g.party_sel_data  = sel_data;
    g.party_sel_count = sel_count;

    // WHAT THE TWO TRIPLES ACTUALLY ARE, thirty-two bytes of them, rather than what the
    // names claim they are. The second vector's offsets are a hypothesis (see kSel_*),
    // and this line is what would falsify it in one look.
    uint8_t raw[32] = {};
    if (mem_read((const uint8_t*)dir + kDir_Party, raw, sizeof(raw))) {
        char hex[3 * 32 + 1] = {};
        for (uint32_t i = 0; i < sizeof(raw); ++i)
            _snprintf_s(hex + i * 3, sizeof(hex) - i * 3, _TRUNCATE, "%02X ", raw[i]);
        log_line("PROBE", "  raw at +%u: %s", (unsigned)kDir_Party, hex);
    }
    log_line("PROBE", "  party  {cap %u, count %u, data 0x%llX}", cap, count,
             (unsigned long long)data);
    log_line("PROBE", "  second {cap %u, count %u, data 0x%llX}   <- offsets ASSUMED,"
                      " the raw line above is the evidence", sel_cap, sel_count,
             (unsigned long long)sel_data);

    // 4 bytes on the counts sidesteps the 8-byte alignment rule; the two pointers are
    // 8-byte aligned by construction (an MSVC vector of u64).
    g.slot[0] = { "party.count", dir + kDir_CatIdCount_, 4 };
    g.slot[1] = { "party.data",  dir + kDir_CatIdData_,  8 };
    g.slot[2] = { "2nd.count",   dir + kSel_Count_,      4 };
    g.slot[3] = { "2nd.data",    dir + kSel_Data_,       8 };
    g.armed = true;

    report_arming("the run's party vector AND the second id vector 16 bytes past it "
                  "(the one the departure code was seen copying between)");
    verify_and_heal();
}

// --- MODE 3: THE CAT SELECTOR'S OBJECT (2026-09-23, route A) ----------------------
//
// WHY THIS ADDRESS. The button probe named it: `CatSelector_Right cb=... rva 0xE91E0`,
// and a static read of that rva shows where the gear screen gets the cat it is editing
// from:
//
//     mov rax, [rcx+8]; inc dword [rax+0x78]      ; the index lives in the object
//     mov r8d, [rcx+0x5BC]                        ; the run's party COUNT (+1468)
//     mov rdx, [r9+0x5C0]; mov rdx, [rdx+rax*8]   ; the run's party DATA (+1472)
//
// Both are reached through pointers the object holds at +8 and +0x10 -- so the screen
// can be aimed at a different list without touching the run, which is exactly what the
// client needs to equip its own (familiar) cats. What this mode does NOT know yet is
// whether those two pointers are read for anything else, which is why the first step is
// to CAPTURE them rather than to overwrite them.
//
// An EXECUTE breakpoint is the only instrument that can answer a READ question, and the
// registers (rcx = the object) exist only inside the trap -- a write watchpoint cannot
// see either. It is armed from the tick, as this module requires, and it is safe here
// where the old append reader was not: that one was a shared hot chunk (27,000 traps a
// second, hung the game), this one is a BUTTON CALLBACK that runs when somebody clicks.
constexpr uint32_t kRva_CatSelectorRight = 0x0E91E0;

// AND THE SAME MODE NOW CARRIES THE PANEL BUILD (2026-09-23). See the note beside
// tune::kCatSelectorProbe: the mode is the mechanism, and what is aimed here is the function
// at base+0x3D5F00 -- the one that fills a level-up screen's option storage by copying
// 0x20-byte templates out of globals, i.e. the panel's own data. Its CALLERS are the answer
// to "who decided this screen is about this cat", because everything the player sees is a
// by-value copy taken at that moment (four runtime instruments established that: the subject
// field holds the drawn cat at NOTHING ELSE, the run object's first 0x800 bytes do not hold
// it, the queue-lookup hook only ever saw strings, and the setters this module hooked are
// generic ones that never receive a cat).
//
// The address is fixed in this build and was verified against its prologue before use; it is
// armed through the module base so it follows the loader wherever the DLL lands.
constexpr uint32_t kRva_PanelBuilder = 0x3D5F00;

// THE THREE FIELDS, AND WHY WATCHING THEM IS THE RIGHT INSTRUMENT (2026-09-23). See the
// declaration in the header for the full reasoning; the short version is that the screen object
// is REUSED (measured: the client's second level-up ran at its first one's address), so a write
// watch on its first qword can never fire, while the fields that hold the panel's content ARE
// written again for every new level-up -- and by then mgmp_choice knows the address.
//
//   0xA0   the subject pointer this module already reads
//   0x360  the option array's begin
//   0x368  the option array's end
//
// A hit's RIP is the instruction that filled the panel; its caller chain is the function that
// decided which cat the panel is about.
static void* g_screen_watch = nullptr;

void listprobe_watch_screen(const void* screen) {
    if (!screen) return;
    if (g_screen_watch != screen) {
        g_screen_watch = const_cast<void*>(screen);
        log_line("PROBE", "screen watch: fields +0xA0/+0x360/+0x368 of the level-up screen at"
                          " 0x%llX are now watched for writes -- the write that fills them"
                          " names the panel's builder, and the screen object is reused, so the"
                          " next level-up is what will trip it",
                 (unsigned long long)(uintptr_t)screen);
    }
}

void selector_tick() {
    // THE EXEC BREAKPOINT IS GONE (2026-09-23). It was aimed at 0x3D5F00 on the theory that
    // that function fills the panel; it never fired during a level-up, and the reuse of the
    // screen object explains part of why: nothing about the panel is built again. The constant
    // stays documented here so the same theory is not re-tried.
    (void)kRva_PanelBuilder;
    if (g_screen_watch) {
        const uint8_t* s = (const uint8_t*)g_screen_watch;
        g.slot[0] = { "lvl screen +0xA0 (subject)",  (uintptr_t)(s + 0xA0),  8, false };
        g.slot[1] = { "lvl screen +0x360 (options)", (uintptr_t)(s + 0x360), 8, false };
        g.slot[2] = { "lvl screen +0x368 (options)", (uintptr_t)(s + 0x368), 8, false };
    } else {
        g.slot[0] = {};
        g.slot[1] = {};
        g.slot[2] = {};
    }
    g.slot[3] = {};
    if (!g.armed || !slots_live()) {
        g.armed = true;
        report_arming("write watches on the level-up screen's own subject and option fields --"
                      " the screen is reused, so the next level-up is what trips them");
    }
    verify_and_heal();
}

void listprobe_tick() {
    if (!g.on) return;

    // MODE 3 takes the whole tick: it arms a code address, so none of the battle-chain
    // walking below applies to it.
    if (tune::kCatSelectorProbe) { selector_tick(); return; }

    // MODE 2 takes the whole tick: see the block above for why the battle chain cannot
    // answer this question at all.
    if (tune::kPartyProbe) { party_tick(); return; }

    // NOT ARMED IS A STATE, AND IT HAS TO SAY SO.
    //
    // The probe's write-watchpoint mode cannot arm until something hands it a
    // TurnControl. When that never happens the log is indistinguishable from a
    // game that was never written to -- which cost a whole play session on
    // 2026-09-21 (the hand-off sat below lockstep's own early return, so a
    // single-player run never reached it). Thirty seconds is long enough that a
    // missing arming is a fact rather than a slow frame.
    if (!g.tc && !tune::kProbeAppend) {
        static uint32_t frames = 0;
        if (++frames == 1800 || (frames % 7200) == 0)
            log_line_lvl(LogLevel::Warn, "PROBE",
                         "!! no TurnControl after %u frames -- the probe is NOT armed and will not "
                         "see anything. Nothing in this log is evidence of absence.", frames);
    }

    if (!g.tc) return;

    void* scene  = nullptr;
    void* sub    = nullptr;
    void* holder = nullptr;
    if (!mem_read((const uint8_t*)g.tc + kTC_Scene, &scene, sizeof(scene)) || !scene) return;
    if (!mem_read((const uint8_t*)scene + kScene_Sub, &sub, sizeof(sub)) || !sub) return;
    if (!mem_read((const uint8_t*)sub + kSub_Holder, &holder, sizeof(holder)) || !holder) return;

    const uintptr_t h = (uintptr_t)holder;

    // THE OTHER HALF OF THE PAIR, AND THE REASON THE PROBE WAS RE-AIMED.
    //
    // `sub` carries two parallel tables: +0x20 holds the live containers, +0x18
    // holds the SOURCE entries the containers are rebuilt from, both indexed the
    // same way. A one-line disassembly of the caller decided this: before reading
    // the battle list out of [sub+0x20]+0x1F90, fn 0x35D0B0 calls
    //
    //     edx = 0x1F9;  rcx = [sub...];  call 0x96B470
    //
    // and 0x96B470(slot 505) is "release the container in that slot, allocate a
    // fresh one, copy [obj+0x18][505] into it, filtering each candidate on three
    // bytes of the candidate". So the SOURCE side is the definition of who is in
    // the battle, the live side is a per-turn copy of it, and the live side is
    // the one every consumer -- and the mod -- reads.
    //
    // Which makes the source the interesting address, not the copy: a per-peer
    // subset is a change to what the source says, and this is the only instrument
    // that can name whatever code writes it.
    void* srcv = nullptr;
    const bool have_src =
        mem_read((const uint8_t*)sub + kSub_Source, &srcv, sizeof(srcv)) && srcv != nullptr;
    const uintptr_t src = have_src ? (uintptr_t)srcv : 0;

    // The live container, read every frame for two reasons: it is the context the
    // hit line prints, and its count is the fourth slot (see below).
    void* vec = nullptr;
    const bool have_vec =
        mem_read((const uint8_t*)h + kHolder_List, &vec, sizeof(vec)) && vec != nullptr;
    const uintptr_t v = have_vec ? (uintptr_t)vec : 0;
    g.vec = v;

    const bool holder_changed = (h != g.holder);
    const bool src_changed    = (src != g.src);
    const bool vec_changed    = (v != g.vec_armed);
    if (!holder_changed && !src_changed && !vec_changed) {
        verify_and_heal();
        return;
    }

    if (holder_changed) {
        g.holder = h;
        // Detection only, so the low half will do -- and 4 bytes sidesteps the
        // 8-byte alignment rule an 8-byte watch would have to satisfy.
        g.slot[0] = { "holder+1F90", h + kHolder_List, 4 };
        g.armed = true;
    }

    if (src_changed) {
        g.src = src;
        if (!src) {
            // A watchpoint watches an ADDRESS. Once the source table is gone the
            // allocator hands those bytes to somebody else and every later write
            // traps, reporting whatever is there now -- one earlier log produced
            // 136 such "hits" with count=6357087, from a module that was not even
            // the game. Disarm rather than watch a tombstone.
            g.slot[1] = {};
            g.slot[2] = {};
        } else {
            // The source entry is INLINE, not a pointer to a container: a plain
            // {cap@+0, count@+4, array@+8} triple. The writer of this triple is
            // the thing that decides who is in the battle, and it is the last
            // unknown left in this question.
            g.slot[1] = { "src.count", src + kHolder_List + 4, 4 };
            g.slot[2] = { "src.array", src + kHolder_List + 8, 8 };
        }
    }

    if (vec_changed) {
        // Slot 3 watches the LIVE list's count, not the source. When the source
        // turns out to be quiet during a battle, this is the line that still says
        // whether the copy ran and how many characters survived its filters --
        // without it, a round with no hits would have two readings ("nothing
        // wrote the source" and "nothing happened at all") and no way to choose.
        g.vec_armed = v;
        g.slot[3] = v ? Slot{ "live.count", v + kList_Count, 4 } : Slot{};
    }

    report_arming(holder_changed
                      ? "the live slot, the SOURCE entry that decides who is in the battle, and the "
                        "live list's count"
                      : "part of the chain moved: re-armed on what replaced it");
    verify_and_heal();
}

// See the note in the header: one slot, borrowed, for the writer of a field that belongs to
// another module's object. Idempotent and cheap -- it programs four registers and says so.
void listprobe_watch_qword(int index, const char* what, const void* addr) {
    if (index < 0 || index > 3) return;
    if (!addr) { g.slot[index] = {}; program_slots(nullptr, nullptr); return; }

    g.slot[index] = Slot{ what, (uintptr_t)addr, 8 };
    g.armed = true;

    uintptr_t rb_dr0 = 0;
    DWORD     rb_dr7 = 0;
    const bool ok = program_slots(&rb_dr0, &rb_dr7);
    log_line("PROBE", "user watch: slot[%d] %s @0x%llX 8 byte(s) write watch -- %s (DR7 0x%X,"
                      " wanted 0x%X). A hit names the RIP that wrote it and unwinds its"
                      " callers through .pdata",
             index, what, (unsigned long long)(uintptr_t)addr, ok ? "armed" : "DID NOT TAKE",
             (unsigned)rb_dr7, (unsigned)build_dr7());
}

// AND THE STEP THAT MAKES IT WORK AT ALL: A HARDWARE BREAKPOINT IS PER THREAD (2026-09-23,
// measured). The first attempt at this armed on the calling thread only, installed cleanly
// (DR7 read back correct), and then never fired -- while the screen was demonstrably on the
// screen. This file already warns about exactly this; the fix is to put the same debug
// registers into every other thread of the process, which is what this does: suspend, set,
// resume, one thread at a time.
void listprobe_arm_other_threads() {
    if (!g.armed) return;

    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        log_line_lvl(LogLevel::Warn, "PROBE",
                     "!! user exec: thread snapshot failed -- only THIS thread carries the"
                     " breakpoint, so another thread's execution will not be seen");
        return;
    }

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    uint32_t armed = 0, refused = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
            if (te.th32ThreadID == self) continue;      // program_slots already did this one

            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
            if (!th) { ++refused; continue; }

            bool ok = false;
            if (SuspendThread(th) != (DWORD)-1) {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(th, &c)) {
                    c.Dr0 = g.slot[0].addr;
                    c.Dr1 = g.slot[1].addr;
                    c.Dr2 = g.slot[2].addr;
                    c.Dr3 = g.slot[3].addr;
                    c.Dr7 = build_dr7();
                    c.Dr6 = 0;
                    ok = SetThreadContext(th, &c) != 0;
                }
                ResumeThread(th);
            }
            CloseHandle(th);
            if (ok) ++armed; else ++refused;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    log_line_lvl((armed || refused) ? LogLevel::Warn : LogLevel::Info, "PROBE",
             "user exec: the same breakpoint was written into %u other thread(s) of this"
             " process (%u refused). A hardware breakpoint only fires on the thread that"
             " carries it -- that is why the first attempt saw nothing at all.",
             armed, refused);
}

// See the header: an execute breakpoint on the instruction that installs a LevelUpScreen's
// vtable. Same one-slot borrow as listprobe_watch_qword, and the same reason it is safe to
// call repeatedly -- the roster tick only rewrites slots when the battle chain moves.
void listprobe_watch_exec(int index, const char* what, const void* addr) {
    if (index < 0 || index > 3) return;
    if (!addr) { g.slot[index] = {}; program_slots(nullptr, nullptr); return; }

    g.slot[index] = Slot{ what, (uintptr_t)addr, 0, /*exec=*/true };
    g.armed = true;

    uintptr_t rb_dr0 = 0;
    DWORD     rb_dr7 = 0;
    const bool ok = program_slots(&rb_dr0, &rb_dr7);
    log_line("PROBE", "user exec breakpoint: slot[%d] %s @0x%llX -- %s (DR7 0x%X, wanted"
                      " 0x%X). A hit names the callers of that instruction",
             index, what, (unsigned long long)(uintptr_t)addr, ok ? "armed" : "DID NOT TAKE",
             (unsigned)rb_dr7, (unsigned)build_dr7());

    // And into every other thread, or the trap will not happen where the screen is built.
    listprobe_arm_other_threads();
}

void listprobe_shutdown() {
    if (!g.on) return;
    g.on = false;
    g.armed = false;
    for (int i = 0; i < 4; ++i) g.slot[i] = {};
    program_slots(nullptr, nullptr);      // writes an empty DR7
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    log_line("PROBE", "disarmed after %u hit(s)", g.total);
}

} // namespace mgmp
