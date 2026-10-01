// mgmp_abandon_game.cpp -- the game half of the abandon vote. See mgmp_abandon.h.
#include "mgmp_abandon.h"

#include <cstdint>
#include <cstring>

#include "mgmp_addresses.h"
#include "mgmp_log.h"
#include "mgmp_resolve.h"

namespace mgmp {
namespace {

// sub_1403C0210 (MewDirector::start_transition): rcx = MewDirector*, rdx = std::string* transition name,
// r8 = std::function<void()>* to run when the fade completes, xmm3 = delay in seconds (the 4th argument is
// a double, so it travels in xmm3 and MSVC gets that right from this prototype).
using fn_start_transition = void(__fastcall*)(void* director, void* name, void* done, double delay);

void* director() {
    const uintptr_t at = addr_of_data(D_MewDirectorPtr);
    return at ? *(void**)at : nullptr;
}

} // namespace

bool abandon_native_ready() {
    return addr_of_call(C_StartTransition) && addr_of_data(D_AbandonFunctionVtable) && director();
}

// Re-creates, by hand, what the popup's OK callback (sub_14029AE10) hands to start_transition:
//   name = the 6-character std::string "defeat"   (MSVC small string: 16 bytes of buffer, size, capacity)
//   done = a std::function whose target is the game's own lambda (vtable read out of the callback), which
//          ends the run the way a lost battle does. MSVC's std::function is 0x40 bytes: the small target
//          in place ([0] vtable, [1] the captured MewDirector*) and, at +0x38, a pointer back to it.
// start_transition copies both before it returns, so stack storage is enough.
bool abandon_run_native() {
    if (!abandon_native_ready()) return false;
    void* dir = director();

    struct alignas(16) Str { char buf[16]; uint64_t size; uint64_t cap; };
    Str name = {};
    memcpy(name.buf, "defeat", 7);
    name.size = 6; name.cap = 15;

    struct alignas(16) Fn { uint64_t w[8]; };
    Fn fn = {};
    fn.w[0] = (uint64_t)addr_of_data(D_AbandonFunctionVtable);
    fn.w[1] = (uint64_t)dir;
    fn.w[7] = (uint64_t)&fn;

    log_line("ABANDON", "starting the game's abandon: start_transition(director %p, \"defeat\")", dir);
    ((fn_start_transition)addr_of_call(C_StartTransition))(dir, &name, &fn, 0.0);
    return true;
}

} // namespace mgmp
