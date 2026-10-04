// mgmp_crash.cpp -- see mgmp_crash.h.
#include "mgmp_crash.h"

#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_page.h"
#include "mgmp_prevrun.h"
#include "mgmp_lockstep.h"
#include "mgmp_catsync.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

constexpr DWORD kCppThrow = 0xE06D7363;   // 'msc' -- the MSVC C++ throw

constexpr int kRingSize   = 24;
constexpr int kMaxFrames  = 16;

struct Record {
    DWORD     code = 0;
    void*     addr = nullptr;
    char      type[96] = {};              // the C++ type name, when we get one
    void*     frames[kMaxFrames] = {};
    USHORT    frame_count = 0;
};

struct State {
    void*     handle = nullptr;
    uintptr_t game_base = 0;
    HMODULE   self = nullptr;
    uintptr_t self_base = 0;

    Record    ring[kRingSize];
    LONG      ring_next = 0;              // monotonic; index is % kRingSize
    LONG      fatal_reported = 0;
    LONG      throws_logged  = 0;
    LONG      guarded_faults = 0;         // caught by mem_read/mem_write
} g;

// How many C++ throws to write to the log AS THEY HAPPEN, rather than only
// ringing them for the unhandled filter.
//
// The filter is not guaranteed a turn. An unhandled C++ exception goes through
// std::terminate -> abort(), which in several CRT configurations never reaches
// SetUnhandledExceptionFilter -- so a ring that is only flushed there can be
// lost exactly when it is needed. Writing the first few immediately means the
// evidence is on disk before anything gets a chance to skip the flush.
//
// Bounded because a game that throws in a loop would otherwise fill the disk,
// and because the interesting throw is near the start of the failure, not
// after ten thousand handled ones.
constexpr LONG kThrowsToLog = 40;

// --- the MSVC throw record --------------------------------------------------
//
// All the pointers inside are 32-bit RVAs relative to the module that threw,
// which x64 passes as ExceptionInformation[3]. Nothing here is documented, but
// it is stable across every MSVC that emits __CxxFrameHandler3/4, and every
// step is bounds-checked because this runs while the process is already sick.
#pragma pack(push, 4)
struct ThrowInfoX      { uint32_t attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray; };
struct CatchableArrayX { int32_t  count; uint32_t types[1]; };
struct CatchableTypeX  { uint32_t properties, pType; };
#pragma pack(pop)

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    // Not a full range check across regions, but enough to stop the common
    // "the pointer is garbage" case from turning a diagnostic into a crash.
    uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (uintptr_t)p + n <= end;
}

// Undecorated-ish: the TypeDescriptor name is a mangled ".?AVfoo@bar@@". The
// leading ".?AV"/".?AU" is stripped and the rest left alone -- enough to read,
// and it avoids dragging in UnDecorateSymbolName from a crashing thread.
void type_name_of(const EXCEPTION_RECORD* er, char* out, size_t out_n) {
    out[0] = 0;
    if (er->NumberParameters < 4) return;

    const ThrowInfoX* ti = (const ThrowInfoX*)er->ExceptionInformation[2];
    uintptr_t         mb = (uintptr_t)er->ExceptionInformation[3];
    if (!ti || !mb || !readable(ti, sizeof(*ti)) || !ti->pCatchableTypeArray) return;

    const CatchableArrayX* arr = (const CatchableArrayX*)(mb + ti->pCatchableTypeArray);
    if (!readable(arr, sizeof(*arr)) || arr->count < 1) return;

    const CatchableTypeX* ct = (const CatchableTypeX*)(mb + arr->types[0]);
    if (!readable(ct, sizeof(*ct)) || !ct->pType) return;

    // TypeDescriptor: { void* vftable; void* spare; char name[]; }
    const char* name = (const char*)(mb + ct->pType) + 16;
    if (!readable(name, 8)) return;

    if (name[0] == '.' && name[1] == '?' && name[2] == 'A' &&
        (name[3] == 'V' || name[3] == 'U'))
        name += 4;

    size_t i = 0;
    for (; i + 1 < out_n && name[i]; ++i) out[i] = name[i];
    out[i] = 0;
}

// --- symbolising ------------------------------------------------------------

// "<module>+<rva>", which is directly comparable with an address in the IDB
// once the game's imagebase (0x140000000) is added back.
void describe(void* addr, char* out, size_t out_n) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) && mod) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t* leaf = path;
        for (const wchar_t* p = path; *p; ++p) if (*p == L'\\' || *p == L'/') leaf = p + 1;

        char name[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, leaf, -1, name, sizeof(name) - 1, nullptr, nullptr);

        uintptr_t rva = (uintptr_t)addr - (uintptr_t)mod;
        // For the game itself also print the address the IDB uses, so a frame
        // can be pasted straight into a disassembler without arithmetic.
        if ((uintptr_t)mod == g.game_base)
            _snprintf_s(out, out_n, _TRUNCATE, "%s+%08llX (ida %012llX)",
                        name, (unsigned long long)rva,
                        (unsigned long long)(0x140000000ull + rva));
        else
            _snprintf_s(out, out_n, _TRUNCATE, "%s+%08llX", name,
                        (unsigned long long)rva);
        return;
    }
    _snprintf_s(out, out_n, _TRUNCATE, "%016llX <no module>", (unsigned long long)addr);
}

bool frame_is_ours(void* addr) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &mod)) return false;
    return mod != nullptr && (uintptr_t)mod == g.self_base;
}

// THE STACK OF THE FAULTING INSTRUCTION, walked from the exception's own CONTEXT (2026-10-03). RtlCaptureStackBackTrace walks the HANDLER's stack, and for an execute fault at address 0 -- a call through a
// null function pointer -- it stops at the dispatcher: the crash report then said "no mgmp.dll frame on this stack" about a stack it had not looked at. From the context: a faulting rip with no unwind info
// (null, or a jump into data) is treated as a call that landed there, so the return address is at [rsp]; every other frame is unwound with the unwind tables. The first entry is the faulting rip itself.
USHORT walk_context(const CONTEXT* in, void** frames, USHORT max) {
    USHORT n = 0;
    __try {
        CONTEXT c = *in;
        frames[n++] = (void*)c.Rip;
        while (n < max) {
            DWORD64 image = 0;
            PRUNTIME_FUNCTION fe = c.Rip ? RtlLookupFunctionEntry(c.Rip, &image, nullptr) : nullptr;
            if (fe) {
                PVOID handler_data = nullptr; DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fe, &c, &handler_data, &frame, nullptr);
            } else {
                DWORD64 ret = 0;
                if (!readable((const void*)c.Rsp, 8)) break;
                ret = *(const DWORD64*)c.Rsp;
                c.Rip = ret; c.Rsp += 8;
            }
            if (!c.Rip) break;
            frames[n++] = (void*)c.Rip;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

USHORT capture_frames(const EXCEPTION_POINTERS* ep, void** frames, USHORT max) {
    if (ep && ep->ContextRecord) {
        const USHORT n = walk_context(ep->ContextRecord, frames, max);
        if (n) return n;
    }
    return RtlCaptureStackBackTrace(1, max, frames, nullptr);
}

// The words on the stack that look like return addresses (they point into a loaded module): a second opinion when the unwinder cannot be trusted, and the only one for a stack the faulting code built by hand.
void dump_stack_scan(const CONTEXT* c) {
    __try {
        const DWORD64* sp = (const DWORD64*)c->Rsp;
        int shown = 0;
        for (int i = 0; i < 96 && shown < 24; ++i) {
            if (!readable(sp + i, 8)) break;
            const DWORD64 v = sp[i];
            if (v < 0x10000) continue;
            HMODULE mod = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &mod) || !mod) continue;
            char at[128];
            describe((void*)v, at, sizeof(at));
            log_line("CRASH", "    [rsp+%03X] %s%s", i * 8, at, frame_is_ours((void*)v) ? "   <-- mgmp" : "");
            ++shown;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// What a register that holds a pointer points at: the first words, and -- when the first word is a pointer into the game -- the vtable's RVA. "rcx = 0x...5288: [vtable -> 0x...]" is how a freed or overwritten
// object shows (its first word is then a heap pointer or garbage instead of a vtable in the exe's image).
void dump_pointer(const char* name, DWORD64 v) {
    if (v < 0x10000 || !readable((const void*)v, 32)) return;
    __try {
        const DWORD64* q = (const DWORD64*)v;
        char vt[96] = "not a game vtable";
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)q[0], &mod) && mod)
            describe((void*)q[0], vt, sizeof(vt));
        log_line("CRASH", "    %s %016llX -> %016llX %016llX %016llX %016llX  [first word: %s]", name, (unsigned long long)v,
                 (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], vt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void dump_pointers(const CONTEXT* c) {
    log_line("CRASH", "  the registers that point at memory:");
    dump_pointer("rax", c->Rax); dump_pointer("rbx", c->Rbx); dump_pointer("rcx", c->Rcx); dump_pointer("rdx", c->Rdx);
    dump_pointer("rsi", c->Rsi); dump_pointer("rdi", c->Rdi); dump_pointer("r8 ", c->R8);  dump_pointer("r9 ", c->R9);
}

void dump_record(const char* why, const Record& r) {
    char at[128];
    describe(r.addr, at, sizeof(at));
    if (r.type[0])
        log_line("CRASH", "%s code %08lX '%s' at %s", why, r.code, r.type, at);
    else
        log_line("CRASH", "%s code %08lX at %s", why, r.code, at);

    bool ours = false;
    for (USHORT i = 0; i < r.frame_count; ++i) {
        char f[128];
        describe(r.frames[i], f, sizeof(f));
        bool mine = frame_is_ours(r.frames[i]);
        if (mine) ours = true;
        log_line("CRASH", "    #%-2u %s%s", (unsigned)i, f, mine ? "   <-- mgmp" : "");
    }
    if (r.frame_count)
        log_line("CRASH", "  %s", ours
                 ? "mgmp.dll IS on this stack -- the mod is in the failing path"
                 : "no mgmp.dll frame on this stack");
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    if (!er) return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = er->ExceptionCode;

    // Debugger/misc noise that is never a fault.
    if (code == DBG_PRINTEXCEPTION_C || code == DBG_PRINTEXCEPTION_WIDE_C ||
        code == 0x406D1388 /* SetThreadName */)
        return EXCEPTION_CONTINUE_SEARCH;

    // A GUARDED READ THAT FAULTED IS NOT A CRASH, AND IT MUST NOT COST THE
    // BUDGET FOR ONE. mem_read exists to survive a pointer whose meaning is a
    // guess, so its access violations are expected traffic -- but they arrive
    // here first, and the four-record cap below was being spent on them. That
    // is the diagnostic failing in exactly the window it was built for: a run
    // that walked a scene list through a teardown wrote four dumps of its own
    // caught reads and then had nothing left to say. Counted, not dumped; the
    // total goes out at shutdown, so "this peer read a lot of dead memory"
    // stays visible without burying anything.
    if (code == EXCEPTION_ACCESS_VIOLATION && mem_guard_active()) {
        InterlockedIncrement(&g.guarded_faults);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    Record r;
    r.code = code;
    r.addr = er->ExceptionAddress;
    if (code == kCppThrow) type_name_of(er, r.type, sizeof(r.type));
    r.frame_count = capture_frames(ep, r.frames, kMaxFrames);

    LONG slot = InterlockedIncrement(&g.ring_next) - 1;
    g.ring[slot % kRingSize] = r;

    // A C++ throw is first-chance noise until proven otherwise -- the game
    // throws and catches on purpose. Anything else at first chance is already
    // a fault, so it is worth saying immediately, in case the process dies
    // before the unhandled filter gets a turn.
    if (code != kCppThrow) {
        if (InterlockedIncrement(&g.fatal_reported) <= 4)
            dump_record("first-chance", r);
    } else {
        LONG n = InterlockedIncrement(&g.throws_logged);
        if (n <= kThrowsToLog) {
            dump_record("first-chance throw", r);
            if (n == kThrowsToLog)
                log_line("CRASH", "  (throw log capped at %ld -- later ones are still"
                                  " ringed and dumped if the process dies)", kThrowsToLog);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// WHAT THE FAULT WAS, for an access violation: read or write, and at which address -- the difference between a null pointer (an address near 0) and a dangling one (a wild address) is the first thing
// a crash in the game's own code has to be asked, and the exception record has it. Plus the registers, so "[rdx]" in a disassembly can be read as a value instead of guessed. Plain formatting only:
// this runs in a process that is going down.
void dump_fault(const EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const ULONG_PTR kind = er->ExceptionInformation[0];
        log_line("CRASH", "  access violation: %s address %016llX%s",
                 kind == 0 ? "READ of" : kind == 1 ? "WRITE to" : kind == 8 ? "EXECUTE at" : "access to",
                 (unsigned long long)er->ExceptionInformation[1],
                 er->ExceptionInformation[1] < 0x10000 ? "  (a NULL pointer plus an offset)" : "  (a wild or freed pointer)");
    }
    if (const CONTEXT* c = ep->ContextRecord) {
        log_line("CRASH", "  rip %016llX rsp %016llX rbp %016llX", (unsigned long long)c->Rip, (unsigned long long)c->Rsp, (unsigned long long)c->Rbp);
        log_line("CRASH", "  rax %016llX rbx %016llX rcx %016llX rdx %016llX", (unsigned long long)c->Rax, (unsigned long long)c->Rbx, (unsigned long long)c->Rcx, (unsigned long long)c->Rdx);
        log_line("CRASH", "  rsi %016llX rdi %016llX r8  %016llX r9  %016llX", (unsigned long long)c->Rsi, (unsigned long long)c->Rdi, (unsigned long long)c->R8, (unsigned long long)c->R9);
        log_line("CRASH", "  r10 %016llX r11 %016llX r12 %016llX r13 %016llX r14 %016llX r15 %016llX", (unsigned long long)c->R10, (unsigned long long)c->R11,
                 (unsigned long long)c->R12, (unsigned long long)c->R13, (unsigned long long)c->R14, (unsigned long long)c->R15);
    }
}

// A MINIDUMP BESIDE THE LOG (same name, .dmp), so a crash that the log alone cannot explain still leaves something to open in a debugger: the faulting thread's stack, the registers and the memory the
// stacks point at. dbghelp is loaded on demand -- nothing here costs anything until the process is already dying -- and any failure is silent, because a diagnostic must not turn into a second crash.
void write_minidump(EXCEPTION_POINTERS* ep) {
    wchar_t path[MAX_PATH] = {};
    if (!log_current_path(path, MAX_PATH)) return;
    wchar_t* dot = wcsrchr(path, L'.');
    if (!dot) return;
    wcscpy_s(dot, (size_t)(path + MAX_PATH - dot), L".dmp");

    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    if (!dbg) return;
    typedef BOOL(WINAPI* WriteDumpFn)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    const WriteDumpFn write = (WriteDumpFn)GetProcAddress(dbg, "MiniDumpWriteDump");
    HANDLE f = write ? CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    if (f != INVALID_HANDLE_VALUE) {
        struct ExcInfo { DWORD thread; EXCEPTION_POINTERS* ptrs; BOOL client; } info = { GetCurrentThreadId(), ep, FALSE };
        constexpr int kWithIndirectlyReferencedMemory = 0x40, kWithThreadInfo = 0x1000;
        // MiniDumpIgnoreInaccessibleMemory (0x20000): without it ONE unreadable page in the memory the dump wants (a guard page, a freed arena) fails the whole dump -- which is how the first report ended up as a
        // 0 KB file. WithUnloadedModules (0x20) names what was loaded. A second, plainer attempt follows a failure, and the error is in the log either way.
        constexpr int kIgnoreInaccessible = 0x20000, kWithUnloadedModules = 0x20;
        BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, kWithIndirectlyReferencedMemory | kWithThreadInfo | kIgnoreInaccessible | kWithUnloadedModules, ep ? &info : nullptr, nullptr, nullptr);
        DWORD err = ok ? 0 : GetLastError();
        if (!ok) {
            log_line("CRASH", "  minidump attempt 1 failed (error 0x%08lX) -- trying a plain one", err);
            SetFilePointer(f, 0, nullptr, FILE_BEGIN); SetEndOfFile(f);
            ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, kIgnoreInaccessible, ep ? &info : nullptr, nullptr, nullptr);
            err = ok ? 0 : GetLastError();
        }
        CloseHandle(f);
        if (ok) log_line("CRASH", "  minidump written: %ls", path);
        else    log_line("CRASH", "  minidump FAILED (error 0x%08lX): %ls", err, path);
    }
}

// WHERE THE SESSION WAS when the process died: the page, the role and who was connected, the battle and turn, the last phase changes (the stage timeline, oldest first, with how long ago each was) and
// the cat bookkeeping (the run's lists, every clone slot). Each read is guarded; the whole thing is one SEH block because a dying process may hold anything in its memory.
void dump_session_context() {
    __try {
        const NetRole role = net_role();
        log_line("CRASH", "  session: page '%s', role %s, %u peer(s) connected, net %s, battle %016llx, turn %u",
                 page_name(page_self()), role == NetRole::Host ? "host" : role == NetRole::Client ? "client" : "none", (unsigned)net_peer_count(),
                 net_active() ? "up" : "down", (unsigned long long)lockstep_battle_id(), (unsigned)log_turn());
        log_line("CRASH", "  last stage: %s", log_stage_last());
        log_stage_dump("CRASH");
        catsync_log_snapshot("crash");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("CRASH", "  (the session context could not be read)");
    }
}

// The crash handler's own upload of the log and dump (mgmp_prevrun): a second fault in here (the process may be in any state) must end this attempt, not the report.
void try_upload_crash_log() {
    __try {
        prevrun_crash_upload();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* ep) {
    // a fault in the upload thread lands here again: the first report is already written, so just let the process go
    static LONG inside = 0;
    if (InterlockedCompareExchange(&inside, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;
    log_line("CRASH", "==== unhandled exception -- the process is going down ====");

    if (ep && ep->ExceptionRecord) {
        Record r;
        r.code = ep->ExceptionRecord->ExceptionCode;
        r.addr = ep->ExceptionRecord->ExceptionAddress;
        if (r.code == kCppThrow) type_name_of(ep->ExceptionRecord, r.type, sizeof(r.type));
        r.frame_count = capture_frames(ep, r.frames, kMaxFrames);
        dump_record("FATAL", r);
        dump_fault(ep);
        if (ep->ContextRecord) {
            dump_pointers(ep->ContextRecord);
            log_line("CRASH", "  words on the faulting stack that point into a module:");
            dump_stack_scan(ep->ContextRecord);
        }
    }
    dump_session_context();

    // The ring, oldest first. For an unhandled C++ throw the stack is already
    // unwound by the time we get here, so the ring's last entry -- captured at
    // first chance, with the throwing frames still live -- is usually the one
    // that actually names the culprit.
    LONG total = g.ring_next;
    LONG first = total > kRingSize ? total - kRingSize : 0;
    if (total > 0)
        log_line("CRASH", "---- %ld exception(s) seen at first chance, most recent last ----",
                 total);
    for (LONG i = first; i < total; ++i) {
        char label[48];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "  [%ld]", i);
        dump_record(label, g.ring[i % kRingSize]);
    }

    if (ep) write_minidump(ep);
    try_upload_crash_log();     // after the dump: the log and the dump are what is sent
    log_shutdown();
    return EXCEPTION_CONTINUE_SEARCH;   // let WER do what it would have done
}

} // namespace

void crash_install(uintptr_t base) {
    if (g.handle) return;
    g.game_base = base;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&crash_install, &g.self);
    g.self_base = (uintptr_t)g.self;

    // First in the chain: we want the throw before anyone else transforms it.
    g.handle = AddVectoredExceptionHandler(1, on_exception);
    SetUnhandledExceptionFilter(on_unhandled);

    log_line("CRASH", "handler armed -- first-chance ring %d deep, %d frames per record;"
                      " a fatal exception dumps module+rva and says whether mgmp.dll is"
                      " on the stack", kRingSize, kMaxFrames);
}

void crash_shutdown() {
    if (!g.handle) return;
    if (g.guarded_faults)
        log_line_lvl(LogLevel::Trace, "CRASH",
                     "%ld guarded read(s) faulted and were caught -- pointers whose "
                     "meaning is a guess, which is what mem_read is for. Not dumped, "
                     "so a real fault still gets the report budget.",
                     g.guarded_faults);
    RemoveVectoredExceptionHandler(g.handle);
    g.handle = nullptr;
}

} // namespace mgmp
