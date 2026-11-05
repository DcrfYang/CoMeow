// Integration tests of the REAL mgmp_choice.cpp and mgmp_mem.cpp. Only the
// network, owner lookup, logging and resolved game callbacks are substituted.
// No DLL, hooks, game process, files or saves are involved.
#include "mgmp_choice.h"
#include "mgmp_follow.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_resolve.h"
#include "mgmp_rtti.h"
#include "mgmp_catsync.h"
#include "mgmp_listprobe.h"
#include "mgmp_net.h"
#include "MinHook.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {
using namespace mgmp;

constexpr size_t kScreenBytes = 0x2200;
constexpr size_t kSubject = 0xA0;
constexpr size_t kCommitted = 792;
constexpr size_t kBegin = 864;
constexpr size_t kEnd = 872;
constexpr size_t kStride = 240;
constexpr size_t kName = 200;
constexpr size_t kSaveId = 0xC48;
constexpr uint32_t kCount = 2;
constexpr uint64_t kNode = 0xFEDCBA9876543210ULL;
constexpr uint64_t kNextNode = 0x1234567876543210ULL; // Same low 32 bits.
constexpr uint64_t kSkippedNode = 0x9988776655443322ULL;
constexpr uint64_t kCat = 0xFEDCBA9889ABCDEFULL;
constexpr uint64_t kOtherCat = 0x13579BDF89ABCDEFULL;
constexpr uint64_t kSeed = 0xEDCBA98776543210ULL;
constexpr uint64_t kOtherSeed = 0x1234567876543210ULL;
constexpr const char* kNames[] = {"str", "lightning_bolt_upgrade"};
constexpr const char* kEventNames[] = {"str", "coins"};
constexpr uint32_t kTypes[] = {1, 4};

static_assert(sizeof(void*) == 8, "The game and capture ABI are x64");
static_assert(sizeof(std::string) == 32,
              "Use MSVC strings with _ITERATOR_DEBUG_LEVEL=0, including Debug builds");
static_assert(kName + sizeof(std::string) <= kStride, "String fits option stride");

int failures = 0;
int checks = 0;
const char* current_test = "fixture";

void check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("  FAIL %s:%d: %s\n", current_test, line, expression);
    }
}
#define CHECK(expr) check(!!(expr), #expr, __LINE__)

// memcpy keeps the synthetic untyped storage free of aliased integer/pointer
// lvalues. Real std::strings are placement-constructed and destroyed in place;
// both the SSO and heap-backed names remain alive throughout every callback.
template<class T> void put(void* memory, size_t offset, const T& value) {
    std::memcpy(static_cast<unsigned char*>(memory) + offset, &value, sizeof(value));
}
template<class T> T get(const void* memory, size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const unsigned char*>(memory) + offset, sizeof(value));
    return value;
}

struct Screen {
    alignas(8) unsigned char bytes[kScreenBytes]{};
    alignas(8) unsigned char cat[kSaveId + sizeof(uint64_t)]{};
    alignas(std::string) unsigned char options[kCount * kStride]{};

    Screen() {
        set_subject(kCat, kSeed);
        unsigned char* subject = cat;
        put(bytes, kSubject, subject);
        unsigned char* begin = options;
        unsigned char* end = options + sizeof(options);
        put(bytes, kBegin, begin);
        put(bytes, kEnd, end);
        put(bytes, 224, begin); // WorldEvent shares this fixture's option array.
        put(bytes, 232, end);
        put(bytes, 0x19F9, uint8_t(1)); // native setupActionChoice ready flag
        new (bytes + 0x1A10) std::string("StacyMutant1");
        for (uint32_t i = 0; i < kCount; ++i) {
            put(option(i), 0, kTypes[i]);
            new (option(i) + kName) std::string(kNames[i]);
            new (option(i) + 64) std::string(kEventNames[i]);
        }
    }
    ~Screen() {
        using String = std::string;
        std::launder(reinterpret_cast<String*>(bytes + 0x1A10))->~String();
        for (uint32_t i = 0; i < kCount; ++i) {
            std::launder(reinterpret_cast<String*>(option(i) + kName))->~String();
            std::launder(reinterpret_cast<String*>(option(i) + 64))->~String();
        }
    }
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;
    unsigned char* option(uint32_t index) { return options + index * kStride; }
    void set_subject(uint64_t id, uint64_t seed) {
        put(cat, kSaveId, id);
        put(cat, 0, seed);
    }
    bool committed() const { return bytes[kCommitted] != 0; }
};

struct Owner { uint64_t id, seed; uint8_t peer; };
struct Commit { void* screen; uint32_t index; };
struct Environment {
    NetRole role = NetRole::None;
    uint8_t self = kNoPeer;
    uint64_t node = 0;
    bool send_ok = true;
    unsigned owner_resets = 0;
    unsigned level_callbacks = 0;
    unsigned event_commits = 0;
    std::vector<Owner> owners;
    std::vector<ChoiceMsg> sends; // Includes failed send attempts.
    std::vector<Commit> commits;
} env;

void own(const Screen& screen, uint8_t peer) {
    const uint64_t id = get<uint64_t>(screen.cat, kSaveId);
    const uint64_t seed = get<uint64_t>(screen.cat, 0);
    for (auto& owner : env.owners) {
        if (owner.id == id && owner.seed == seed) { owner.peer = peer; return; }
    }
    env.owners.push_back({id, seed, peer});
}

bool level_detour(void* screen, void* option) {
    if (!choice_on_level_select(screen, option)) return false;
    // The original game's effects are intentionally NOT reimplemented. Its
    // successful commit is represented only by the committed byte and a count.
    put(screen, kCommitted, uint8_t(1));
    auto* begin = get<unsigned char*>(screen, kBegin);
    const auto index = static_cast<uint32_t>(
        (static_cast<unsigned char*>(option) - begin) / kStride);
    env.commits.push_back({screen, index});
    return true;
}

void __fastcall fake_level_click(void* capture) {
    ++env.level_callbacks;
    void* screen = get<void*>(capture, 8);
    const int32_t index = get<int32_t>(capture, 16); // NOT an option pointer.
    CHECK(screen != nullptr);
    CHECK(index >= 0 && index < static_cast<int32_t>(kCount));
    if (!screen || index < 0 || index >= static_cast<int32_t>(kCount)) return;
    auto* begin = get<unsigned char*>(screen, kBegin);
    CHECK(level_detour(screen, begin + kStride * index));
}

void __fastcall fake_event_commit(void* capture) {
    void* screen = get<void*>(capture, 8);
    void* option = get<void*>(capture, 16);
    const bool ok = choice_on_event_commit(capture);
    // The game mutates these fields in its native commit.  Keeping that
    // mutation in the fixture lets the client-side acknowledgement check
    // exercise the same state transition as the real callback.
    if (ok && screen && option) {
        put(screen, 0x100, option);
        put(screen, 0x19F9, uint8_t(0));
    }
    if (ok) ++env.event_commits;
}

bool local_click(Screen& screen, uint32_t index = 1) {
    return level_detour(screen.bytes, screen.option(index));
}

bool local_event(Screen& screen, uint32_t index = 1) {
    void* capture[] = {nullptr, screen.bytes, screen.option(index)};
    const unsigned before = env.event_commits;
    fake_event_commit(capture);
    return env.event_commits == before + 1;
}

void enter(uint64_t node) {
    env.node = node;
    choice_on_node_entered(node);
}

struct Fixture {
    Screen first, second, third;
    explicit Fixture(uint8_t self = kHostPeer) {
        choice_shutdown();
        env = Environment{};
        env.self = self;
        env.role = self == kHostPeer ? NetRole::Host : NetRole::Client;
        choice_set_base(0x140000000ULL);
        choice_init();
        choice_reset_run();
        enter(kNode);
    }
    ~Fixture() {
        // Clear all cached pointers before Screen destroys any of its strings.
        choice_reset_run();
        choice_shutdown();
    }
};

ChoiceMsg level_message(const Screen& screen, uint32_t step = 0,
                        uint32_t index = 1, uint64_t node = kNode) {
    ChoiceMsg message{};
    message.kind = kChoiceLevelUp;
    message.node_seed = node;
    message.cat_id = get<uint64_t>(screen.cat, kSaveId);
    message.cat_seed = get<uint64_t>(screen.cat, 0);
    message.level_step = step;
    message.index = index;
    message.count = kCount;
    message.aux = kTypes[index];
    std::snprintf(message.name, sizeof(message.name), "%s", kNames[index]);
    return message;
}

void check_message(const ChoiceMsg& actual, const ChoiceMsg& expected) {
    CHECK(actual.kind == expected.kind);
    CHECK(actual.node_seed == expected.node_seed);
    CHECK(actual.cat_id == expected.cat_id);
    CHECK(actual.cat_seed == expected.cat_seed);
    CHECK(actual.level_step == expected.level_step);
    CHECK(actual.index == expected.index);
    CHECK(actual.count == expected.count);
    CHECK(actual.aux == expected.aux);
    CHECK(std::strcmp(actual.name, expected.name) == 0);
}

void test_local_ownership_and_identity() {
        CHECK(kProtoVersion >= 44);   // the party order in ENTERNODE arrived with 44
    for (uint8_t self : {uint8_t(0), uint8_t(1)}) {
        Fixture f(self);
        f.second.set_subject(kOtherCat, kOtherSeed);
        own(f.first, kHostPeer);
        own(f.second, 1);
        Screen& mine = self == 0 ? f.first : f.second;
        Screen& theirs = self == 0 ? f.second : f.first;
        CHECK(!local_click(theirs));
        CHECK(!theirs.committed());
        CHECK(env.sends.empty());
        CHECK(env.commits.empty());
        CHECK(local_click(mine));
        CHECK(mine.committed());
        CHECK(env.commits.size() == 1);
        CHECK(env.sends.size() == 1);
        if (env.sends.size() == 1) check_message(env.sends[0], level_message(mine));
        CHECK(!local_click(mine, 0));
        CHECK(env.sends.size() == 1); // A second click cannot publish again.
        CHECK(env.commits.size() == 1);

        // Repeated notification of the same node is not a new upgrade sequence.
        enter(kNode);
        f.third.set_subject(get<uint64_t>(mine.cat, kSaveId), get<uint64_t>(mine.cat, 0));
        CHECK(local_click(f.third, 0));
        CHECK(env.sends.size() == 2);
        if (env.sends.size() == 2) check_message(env.sends[1], level_message(f.third, 1, 0));
    }
}

void test_send_failure_is_not_a_commit() {
    for (uint8_t self : {uint8_t(0), uint8_t(1)}) {
        Fixture f(self);
        own(f.first, self);
        env.send_ok = false;
        CHECK(!local_click(f.first));
        CHECK(!f.first.committed());
        CHECK(env.commits.empty());
        CHECK(env.sends.size() == 1);
        env.send_ok = true;
        CHECK(local_click(f.first));
        CHECK(env.sends.size() == 2);
        if (env.sends.size() == 2) {
            check_message(env.sends[0], level_message(f.first));
            check_message(env.sends[1], level_message(f.first)); // Still step 0.
        }
        CHECK(env.commits.size() == 1);
        CHECK(!local_click(f.first));
        CHECK(env.sends.size() == 2);
    }
}

void test_remote_requires_actual_update() {
    for (uint8_t self : {uint8_t(0), uint8_t(1)}) {
        for (bool cached : {false, true}) {
            Fixture f(self);
            const uint8_t from = self ^ 1;
            own(f.first, from);
            if (cached) choice_on_level_update(f.first.bytes);
            ChoiceMsg message = level_message(f.first);
            choice_on_message(from, message);
            CHECK(env.level_callbacks == 0);
            CHECK(env.commits.empty());
            CHECK(!f.first.committed());
            // Receipt owns a copy, not the caller's temporary message storage.
            message.index = 0;
            message.name[0] = 'x';
            choice_on_level_update(f.first.bytes);
            CHECK(env.level_callbacks == 1);
            CHECK(env.commits.size() == 1);
            CHECK(f.first.committed());
            if (!env.commits.empty()) {
                CHECK(env.commits[0].screen == f.first.bytes);
                CHECK(env.commits[0].index == 1);
            }
            choice_on_level_update(f.first.bytes);
            CHECK(env.level_callbacks == 1);
            CHECK(env.sends.empty()); // The injected detour must not echo.
        }
    }
}

void test_invalid_sender_and_non_owner() {
    for (uint8_t self : {uint8_t(0), uint8_t(1)}) {
        for (uint8_t from : {self, kMaxPeers, kNoPeer, uint8_t(2)}) {
            Fixture f(self);
            // Isolate sender rejection from owner mismatch for self/out-of-range.
            own(f.first, from == 2 ? uint8_t(self ^ 1) : from);
            choice_on_message(from, level_message(f.first));
            choice_on_level_update(f.first.bytes);
            CHECK(!f.first.committed());
            CHECK(env.level_callbacks == 0);
            CHECK(env.commits.empty());
            CHECK(env.sends.empty());
        }
        Fixture f(self);
        own(f.first, self); // Even the host cannot decide a client's upgrade.
        choice_on_message(self ^ 1, level_message(f.first));
        choice_on_level_update(f.first.bytes);
        CHECK(!f.first.committed());
        CHECK(env.level_callbacks == 0);
        CHECK(local_click(f.first));
        CHECK(env.sends.size() == 1);
        if (!env.sends.empty()) CHECK(env.sends[0].level_step == 0);
    }
}

void test_mismatched_messages_do_not_apply() {
    enum Mismatch { Node, CatId, CatSeed, Type, Name, Count, Index, EmptyCount,
                    HugeCount, NoNode, NoCat, InvalidCat, Kind, FutureStep, End };
    const char* labels[] = {"node", "cat id", "cat seed", "type", "name", "count",
        "index", "empty count", "huge count", "no node", "no cat", "invalid cat",
        "kind", "future step"};
    for (uint8_t self : {uint8_t(0), uint8_t(1)}) {
        for (int kind = Node; kind != End; ++kind) {
            Fixture f(self);
            own(f.first, self ^ 1);
            auto message = level_message(f.first);
            switch (kind) {
            case Node: message.node_seed ^= 1ULL << 48; break;
            case CatId: message.cat_id ^= 1ULL << 48; break;
            case CatSeed: message.cat_seed ^= 1ULL << 48; break;
            case Type: ++message.aux; break;
            case Name: message.name[0] = 'X'; break;
            case Count: ++message.count; break;
            case Index: message.index = message.count; break;
            case EmptyCount: message.count = 0; break;
            case HugeCount: message.count = 65; break;
            case NoNode: message.node_seed = 0; break;
            case NoCat: message.cat_id = 0; break;
            case InvalidCat: message.cat_id = UINT64_MAX; break;
            case Kind: message.kind = kChoiceLevelUp + 1; break;
            case FutureStep: message.level_step = 1; break;
            }
            const int before = failures;
            choice_on_level_update(f.first.bytes);
            choice_on_message(self ^ 1, message);
            choice_on_level_update(f.first.bytes);
            choice_on_level_update(f.first.bytes);
            CHECK(!f.first.committed());
            CHECK(env.level_callbacks == 0);
            CHECK(env.commits.empty());
            CHECK(env.sends.empty());
            if (failures != before) std::printf("    mismatch=%s self=%u\n", labels[kind], self);
        }
    }
}

void test_unknown_owner_holds_until_available() {
    for (uint8_t self : {uint8_t(0), uint8_t(1)}) {
        Fixture f(self);
        CHECK(!local_click(f.first));
        CHECK(env.sends.empty());
        choice_on_message(self ^ 1, level_message(f.first));
        choice_on_level_update(f.first.bytes);
        choice_on_level_update(f.first.bytes);
        CHECK(!f.first.committed());
        CHECK(env.level_callbacks == 0);
        own(f.first, self ^ 1);
        CHECK(env.level_callbacks == 0);
        choice_on_level_update(f.first.bytes); // No second message required.
        CHECK(f.first.committed());
        CHECK(env.level_callbacks == 1);
        CHECK(env.commits.size() == 1);
        CHECK(env.sends.empty());
    }
}

void test_owner_is_rechecked_after_hold() {
    Fixture f;
    const auto message = level_message(f.first);
    choice_on_message(2, message); // Ownership was not known on receipt.
    choice_on_level_update(f.first.bytes);
    CHECK(env.level_callbacks == 0);
    own(f.first, 1);
    choice_on_level_update(f.first.bytes);
    CHECK(env.level_callbacks == 0);
    choice_on_message(1, message); // The rejected claim must not poison step 0.
    choice_on_level_update(f.first.bytes);
    CHECK(f.first.committed());
    CHECK(env.level_callbacks == 1);
    CHECK(env.sends.empty());
}

void test_early_next_node_choice_survives() {
    for (uint8_t self : {uint8_t(0), uint8_t(1)}) {
        Fixture f(self);
        own(f.first, self ^ 1);
        choice_on_level_update(f.first.bytes);
        choice_on_message(self ^ 1, level_message(f.second, 0, 1, kNextNode));
        choice_on_level_update(f.first.bytes); // Same cat/options, wrong node.
        CHECK(env.level_callbacks == 0);
        enter(kNextNode);
        CHECK(env.level_callbacks == 0); // Node entry is not a UI update either.
        CHECK(!f.first.committed());
        choice_on_level_update(f.second.bytes);
        CHECK(f.second.committed());
        CHECK(!f.first.committed());
        CHECK(env.level_callbacks == 1);
        if (!env.commits.empty()) CHECK(env.commits[0].screen == f.second.bytes);
        CHECK(env.sends.empty());
    }
}

void test_two_upgrades_queue_and_stale_steps() {
    for (uint8_t self : {uint8_t(0), uint8_t(1)}) {
        for (bool reversed : {false, true}) {
            Fixture f(self);
            const uint8_t from = self ^ 1;
            own(f.first, from);
            const auto first = level_message(f.first, 0, 0);
            const auto second = level_message(f.second, 1, 1);
            if (reversed) {
                choice_on_message(from, second);
                choice_on_level_update(f.first.bytes);
                CHECK(env.level_callbacks == 0); // Cannot skip step 0.
            }
            choice_on_message(from, first);
            choice_on_message(from, first); // Duplicate while still queued.
            choice_on_message(from, second);
            CHECK(env.level_callbacks == 0);
            choice_on_level_update(f.first.bytes);
            CHECK(f.first.committed());
            CHECK(!f.second.committed());
            CHECK(env.level_callbacks == 1);
            if (!env.commits.empty()) CHECK(env.commits[0].index == 0);

            choice_on_message(from, first); // Completed step must not replay.
            choice_on_level_update(f.first.bytes);
            choice_on_level_update(f.first.bytes);
            CHECK(env.level_callbacks == 1); // Old committed screen cannot eat step 1.
            choice_on_level_update(f.second.bytes);
            CHECK(f.second.committed());
            CHECK(env.level_callbacks == 2);
            CHECK(env.commits.size() == 2);
            if (env.commits.size() == 2) {
                CHECK(env.commits[1].screen == f.second.bytes);
                CHECK(env.commits[1].index == 1);
            }
            choice_on_message(from, first);
            choice_on_message(from, second);
            choice_on_level_update(f.third.bytes);
            CHECK(!f.third.committed());
            CHECK(env.level_callbacks == 2);
            CHECK(env.sends.empty());
        }
    }
}

void test_sequence_is_shared_by_local_and_remote() {
    for (uint8_t self : {uint8_t(0), uint8_t(1)}) {
        Fixture f(self);
        f.second.set_subject(kOtherCat, kOtherSeed);
        own(f.first, self);
        own(f.second, self ^ 1);
        CHECK(local_click(f.first));
        choice_on_message(self ^ 1, level_message(f.second, 0));
        choice_on_level_update(f.second.bytes);
        CHECK(!f.second.committed());
        choice_on_message(self ^ 1, level_message(f.second, 1));
        CHECK(env.level_callbacks == 0);
        choice_on_level_update(f.second.bytes);
        CHECK(f.second.committed());
        CHECK(env.level_callbacks == 1);
        CHECK(local_click(f.third));
        CHECK(env.sends.size() == 2);
        if (env.sends.size() == 2) {
            CHECK(env.sends[0].level_step == 0);
            CHECK(env.sends[1].level_step == 2);
        }
    }
}

void test_new_run_clears_queue_step_and_owners() {
    Fixture f;
    own(f.first, 1);
    choice_on_message(1, level_message(f.first));
    choice_on_level_update(f.first.bytes);
    CHECK(env.level_callbacks == 1);
    choice_on_message(1, level_message(f.second, 1));
    choice_on_message(1, level_message(f.third, 0, 1, kNextNode));
    const unsigned resets = env.owner_resets;
    choice_reset_run();
    env.node = 0;
    CHECK(env.owner_resets == resets + 1);
    CHECK(env.owners.empty());
    CHECK(!local_click(f.second));
    CHECK(env.sends.empty());
    enter(kNode);
    CHECK(!local_click(f.second)); // Entering alone does not restore ownership.
    choice_on_level_update(f.second.bytes);
    CHECK(env.level_callbacks == 1);
    own(f.second, 1);
    choice_on_message(1, level_message(f.second)); // Step 0 is valid in a new run.
    CHECK(env.level_callbacks == 1);
    choice_on_level_update(f.second.bytes);
    CHECK(f.second.committed());
    CHECK(env.level_callbacks == 2);
    choice_on_level_update(f.third.bytes);
    CHECK(!f.third.committed()); // Pre-reset step 1 was cleared, not just hidden.
    enter(kNextNode);
    choice_on_level_update(f.third.bytes);
    CHECK(!f.third.committed()); // Pre-reset future-node queue was cleared too.
    CHECK(env.level_callbacks == 2);
    own(f.third, 0);
    CHECK(local_click(f.third));
    CHECK(env.sends.size() == 1);
    if (!env.sends.empty()) check_message(env.sends[0], level_message(f.third, 0, 1, kNextNode));
}

void test_skipped_clears_queued_and_late_choices() {
    for (bool queued_before_skip : {false, true}) {
        Fixture f;
        own(f.first, 1);
        const auto message = level_message(f.first, 0, 1, kSkippedNode);
        if (queued_before_skip) {
            choice_on_message(1, message);
            choice_on_message(1, level_message(f.second, 1, 0, kSkippedNode));
        }
        choice_on_node_skipped(kSkippedNode);
        choice_on_message(1, message); // Late receipt must also be refused.
        enter(kSkippedNode);
        choice_on_level_update(f.first.bytes);
        CHECK(!f.first.committed());
        CHECK(env.level_callbacks == 0);
        enter(kNextNode);
        choice_on_message(1, level_message(f.second, 0, 1, kNextNode));
        choice_on_level_update(f.second.bytes);
        CHECK(f.second.committed());
        CHECK(env.level_callbacks == 1);

        choice_reset_run(); // Skipped-node tombstones are run-scoped.
        own(f.third, 1);
        enter(kSkippedNode);
        choice_on_message(1, level_message(f.third, 0, 1, kSkippedNode));
        choice_on_level_update(f.third.bytes);
        CHECK(f.third.committed());
        CHECK(env.level_callbacks == 2);
        CHECK(env.sends.empty());
    }
}

void test_unknown_sender_cannot_exhaust_other_owner_queue() {
    Fixture f(2);
    for (uint32_t step = 0; step < 65; ++step)
        choice_on_message(1, level_message(f.first, step));
    choice_on_message(kHostPeer, level_message(f.first));
    own(f.first, kHostPeer);
    choice_on_level_update(f.first.bytes);
    CHECK(f.first.committed());
    CHECK(env.commits.size() == 1);
    CHECK(env.sends.empty());
}

void test_same_node_notice_preserves_step_and_rearms_after_reset() {
    Fixture f;
    own(f.first, kHostPeer);
    own(f.second, kHostPeer);
    CHECK(local_click(f.first));
    enter(kNode);
    CHECK(local_click(f.second));
    CHECK(env.sends.back().level_step == 1);
    choice_reset_run();
    own(f.third, kHostPeer);
    enter(kNode);
    CHECK(local_click(f.third));
    CHECK(env.sends.back().level_step == 0);
}

void test_events_remain_host_only() {
    ChoiceMsg event{};
    event.kind = kChoiceEvent;
    event.node_seed = kNode;
    event.index = 1;
    event.count = kCount;
    std::snprintf(event.name, sizeof(event.name), "%s", kEventNames[1]);
    std::snprintf(event.event_name, sizeof(event.event_name), "StacyMutant1");
    {
        Fixture f(0);
        enter(kNode);
        own(f.first, 1); // Event authority is not derived from cat ownership.
        choice_on_event_update(f.first.bytes);
        choice_on_message(1, event);
        choice_on_event_update(f.first.bytes);
        CHECK(env.event_commits == 0);
        CHECK(local_event(f.first));
        CHECK(env.event_commits == 1);
        CHECK(env.sends.size() == 1);
        if (!env.sends.empty()) check_message(env.sends[0], event);
    }
    {
        Fixture f(1);
        enter(kNode);
        own(f.first, 1);
        CHECK(!local_event(f.first));
        CHECK(env.sends.empty());
        choice_on_event_update(f.first.bytes);
        for (uint8_t from : {uint8_t(1), uint8_t(2), kNoPeer}) {
            choice_on_message(from, event);
            choice_on_event_update(f.first.bytes);
            CHECK(env.event_commits == 0);
        }
        choice_on_message(kHostPeer, event);
        choice_on_event_update(f.first.bytes);
        CHECK(env.event_commits == 1);
        CHECK(env.sends.empty());
        CHECK(env.level_callbacks == 0);
    }
}

void test_fixture_uses_real_memory_and_string_layout() {
    Screen screen;
    for (uint32_t i = 0; i < kCount; ++i) {
        char name[48]{};
        CHECK(mem_read_std_string(screen.option(i) + kName, name, sizeof(name)));
        CHECK(std::strcmp(name, kNames[i]) == 0);
        CHECK(get<uint64_t>(screen.option(i), kName + 16) == std::strlen(kNames[i]));
        CHECK((get<uint64_t>(screen.option(i), kName + 24) > 15) == (i == 1));
    }
}

} // namespace

// Include the production headers above so these stubs cannot silently acquire
// signatures different from the real dependencies. Do not stub mem_read.
namespace mgmp {
NetRole net_role() { return env.role; }
uint8_t net_self() { return env.self; }
uint8_t net_peer_pos() { return env.self; }
uintptr_t addr_of_data(DataSym) { return 0; }
bool lockstep_owner_pos(uint64_t id, uint8_t& owner) {
    for (const auto& e : env.owners) if (e.id == id) { owner = e.peer; return true; }
    owner = kNoPeer; return false;
}
bool lockstep_cat_is_mine(uint64_t id, bool& mine) {
    uint8_t owner = kNoPeer; const bool ok = lockstep_owner_pos(id, owner);
    mine = ok && owner == env.self; return ok;
}
bool catsync_publish(const char*, bool, bool) { return true; }
void listprobe_watch_screen(const void*) {}
bool net_send_choice(const ChoiceMsg& message) {
    env.sends.push_back(message);
    return env.send_ok;
}
uint64_t follow_here_seed() { return env.node; }
bool lockstep_cat_owner(uint64_t save_id, uint64_t seed, uint8_t& owner) {
    owner = kNoPeer;
    for (const auto& entry : env.owners) {
        if (entry.id == save_id && entry.seed == seed) { owner = entry.peer; return true; }
    }
    return false;
}
void lockstep_reset_cat_owners() { env.owners.clear(); ++env.owner_resets; }
void lockstep_probe_roster_ids(const void*) {}
void follow_reset_run() {}
uintptr_t addr_of(Target target) {
    return target == T_EventChoice ? reinterpret_cast<uintptr_t>(&fake_event_commit) : 0;
}
uintptr_t addr_of_call(Call call) {
    return call == C_LevelUpClick ? reinterpret_cast<uintptr_t>(&fake_level_click) : 0;
}
const char* rtti_class_name(const void*, char* buffer, size_t size) {
    if (buffer && size) std::snprintf(buffer, size, "test::LevelUpScreen");
    return buffer;
}
void log_line(const char*, const char*, ...) {}
void log_line_lvl(LogLevel, const char*, const char*, ...) {}
} // namespace mgmp

extern "C" MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID*) { return MH_ERROR_NOT_INITIALIZED; }
extern "C" MH_STATUS WINAPI MH_EnableHook(LPVOID) { return MH_ERROR_NOT_INITIALIZED; }

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"fixture memory/string ABI", test_fixture_uses_real_memory_and_string_layout},
        {"local ownership and 64-bit identity", test_local_ownership_and_identity},
        {"failed send and retry", test_send_failure_is_not_a_commit},
        {"remote waits for actual update", test_remote_requires_actual_update},
        {"invalid sender and non-owner", test_invalid_sender_and_non_owner},
        {"mismatched messages", test_mismatched_messages_do_not_apply},
        {"unknown owner holds", test_unknown_owner_holds_until_available},
        {"owner rechecked after hold", test_owner_is_rechecked_after_hold},
        {"early next-node message", test_early_next_node_choice_survives},
        {"two queued upgrades and stale steps", test_two_upgrades_queue_and_stale_steps},
        {"shared local/remote sequence", test_sequence_is_shared_by_local_and_remote},
        {"new-run reset", test_new_run_clears_queue_step_and_owners},
        {"skipped-node cleanup", test_skipped_clears_queued_and_late_choices},
        {"sender queue isolation", test_unknown_sender_cannot_exhaust_other_owner_queue},
        {"same-node reload and sequence", test_same_node_notice_preserves_step_and_rearms_after_reset},
        {"host-only events", test_events_remain_host_only},
    };
    for (const auto& test : tests) {
        current_test = test.name;
        const int before = failures;
        test.run();
        std::printf("  %s  %s\n", failures == before ? "PASSED" : "FAILED", test.name);
    }
    std::printf("test_choice: %d checks, %d failures -- %s\n",
                checks, failures, failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
