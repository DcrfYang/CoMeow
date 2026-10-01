// test_tables.cpp -- the parallel tables are indexed by enum, so the ORDER of their entries is load-bearing.
//
// 2026-10-01 live crash: a new target was appended to kTargets (enum Target / mgmp_addresses.h) but its signature was
// inserted one slot EARLIER in kTargetSigs (mgmp_sigs.generated.h). Everything still compiled (the length check
// passed), every signature still resolved and was unique, and the two hooks silently swapped targets: the "defeat" hook
// sat on Character::load and the loader hook on the end-of-run function, so the first battle crashed. Nothing checked
// that entry i of one table describes the same function as entry i of the other. This does.
//
//   kTargets[i]  = { rva, "NAME", "symbol" }            kTargetSigs[i] = { rva_hint, "NAME", pattern }
//   kCalls[i]    = { rva, "description" }               kCallSigs[i]   = { rva_hint, "Name", pattern }
#include "../src/core/mgmp_addresses.h"
#include "../src/core/mgmp_resolve.h"
#include "../src/core/mgmp_sigs.generated.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace mgmp;
static unsigned checks = 0, failures = 0;
#define CHECK(x, ...) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %d: %s -- ", __LINE__, #x); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    static_assert(sizeof(kTargetSigs) / sizeof(kTargetSigs[0]) == (size_t)T_COUNT, "target sig count");
    static_assert(sizeof(kCallSigs) / sizeof(kCallSigs[0]) == (size_t)C_COUNT, "call sig count");

    std::set<std::string> names;
    for (int i = 0; i < T_COUNT; ++i) {
        const TargetDesc& t = kTargets[i];
        const SigTargetDesc& s = kTargetSigs[i];
        CHECK(t.rva == s.rva_hint, "target #%d: kTargets says %s @0x%X but the signature at that index is %s @0x%X",
              i, t.name, t.rva, s.name, s.rva_hint);
        CHECK(!strcmp(t.name, s.name), "target #%d: kTargets '%s' vs kTargetSigs '%s'", i, t.name, s.name);
        CHECK(s.pattern && s.pattern[0], "target #%d (%s) has an empty pattern", i, s.name);
        CHECK(names.insert(t.name).second, "target #%d: duplicate name %s", i, t.name);
    }
    std::set<uint32_t> rvas;
    for (int i = 0; i < T_COUNT; ++i)
        CHECK(rvas.insert(kTargets[i].rva).second, "target #%d (%s): another target already has rva 0x%X", i, kTargets[i].name, kTargets[i].rva);
    for (int i = 0; i < C_COUNT; ++i)
        CHECK(kCalls[i].rva == kCallSigs[i].rva_hint, "call #%d: kCalls says '%s' @0x%X but the signature at that index is %s @0x%X",
              i, kCalls[i].name, kCalls[i].rva, kCallSigs[i].name, kCallSigs[i].rva_hint);
    static_assert(sizeof(kSigData) / sizeof(kSigData[0]) == (size_t)D_COUNT, "data sig count");
    const struct { int d; const char* name; } data_order[] = {
        { D_MewDirectorPtr, "MewDirectorPtr" }, { D_MouseCache, "MouseCache" }, { D_ApplicationBase, "ApplicationBase" },
        { D_CatIdCounter, "CatIdCounter" }, { D_CatRegister, "CatRegister" }, { D_AbandonFunctionVtable, "AbandonFunctionVtable" },
    };
    CHECK(sizeof(data_order) / sizeof(data_order[0]) == (size_t)D_COUNT, "the data list here is out of date (%d enumerators)", (int)D_COUNT);
    for (const auto& e : data_order)
        CHECK(!strcmp(kSigData[e.d].name, e.name), "data #%d: enum says %s, kSigData says %s", e.d, e.name, kSigData[e.d].name);
    std::printf("tables: %d targets, %d calls, %u checks, %u failures\n", (int)T_COUNT, (int)C_COUNT, checks, failures);
    return failures ? 1 : 0;
}
