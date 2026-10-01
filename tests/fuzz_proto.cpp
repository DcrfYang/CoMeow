// fuzz_proto.cpp -- every wire decoder against random and mutated input, built with AddressSanitizer.
//
// A joined peer is a stranger on the network: whatever it sends reaches these decoders before anything has
// validated it. The property tested here is only "never reads or writes out of bounds, never crashes, never
// allocates absurdly" -- not that the message means anything. Run:
//     _buildcheck\fuzz_proto.bat           (cl /fsanitize=address)
#include "mgmp_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace mgmp;

static std::mt19937_64 rng(0x5eed);
static unsigned long long runs = 0, accepted = 0;

template <class Msg, class Fn> static void fuzz_one(const char* name, Fn dec, unsigned iterations) {
    unsigned long long ok = 0;
    for (unsigned it = 0; it < iterations; ++it) {
        const uint32_t len = (uint32_t)(rng() % ((it & 7) == 0 ? 6000 : 300));
        std::vector<uint8_t> b(len ? len : 1);
        for (auto& x : b) x = (uint8_t)rng();
        // Half of the inputs are mostly small numbers, so length fields land inside the buffer and the interesting
        // branches (sizes, counts) are reached; the other half is pure noise.
        if (it & 1) for (auto& x : b) if (rng() % 3) x &= 0x07;
        Reader r(b.data(), len);
        Msg m{};
        if (dec(r, m)) ++ok;
        ++runs;
    }
    accepted += ok;
    std::printf("  %-12s %u inputs, %llu accepted\n", name, iterations, ok);
}

// A decoded message that frees what it owns, so the harness itself does not run out of memory.
static void drop(CatDataMsg& m) { free(m.data); m.data = nullptr; }

int main(int argc, char** argv) {
    const unsigned N = argc > 1 ? (unsigned)atoi(argv[1]) : 40000;
    std::printf("fuzzing the decoders, %u inputs each\n", N);
#define F(Type, name, fn) fuzz_one<Type>(name, [](Reader& r, Type& m) { return fn(r, m); }, N)
    F(Hello, "hello", dec_hello);
    F(Welcome, "welcome", dec_welcome);
    F(ActionMsg, "action", dec_action);
    F(HashMsg, "hash", dec_hash);
    F(SetupMsg, "setup", dec_setup);
    F(CatDataMsg, "catdata", dec_catdata);
    F(InventoryMsg, "inventory", dec_inventory);
    F(RunHistMsg, "runhist", dec_runhist);
    F(NodeHashMsg, "nodehash", dec_nodehash);
    F(ControlMsg, "control", dec_control);
    F(PeersMsg, "peers", dec_peers);
    F(AimMsg, "aim", dec_aim);
    F(CursorMsg, "cursor", dec_cursor);
    F(EnterNodeMsg, "enternode", dec_enter_node);
    F(PartyMsg, "party", dec_party);
    F(DebugHitMsg, "debughit", dec_debughit);
    F(ChoiceMsg, "choice", dec_choice);
    F(SaveFileMsg, "savefile", dec_savefile);
    F(ChapterMapMsg, "chaptermap", dec_chaptermap);
    F(HostLeftMsg, "hostleft", dec_hostleft);
    F(HaltMsg, "halt", dec_halt);
    F(StateDumpMsg, "statedump", dec_statedump);
    F(ChapterMsg, "chapter", dec_chapter);
    F(ChapterSeedMsg, "chapterseed", dec_chapterseed);
    F(CatDigestMsg, "catdigest", dec_catdigest);
    F(CheckpointMsg, "checkpoint", dec_checkpoint);
    F(PageMsg, "page", dec_page);
    F(CatsMsg, "cats", dec_cats);
    F(SaveWaitMsg, "savewait", dec_savewait);
    F(AbandonMsg, "abandon", dec_abandon);
    F(RoomCtlMsg, "roomctl", dec_roomctl);
#undef F
    (void)drop;
    std::printf("fuzz: %llu inputs, %llu accepted, no fault\n", runs, accepted);
    return 0;
}
