#include "mgmp_derived.h"
#include <cstdio>
using namespace mgmp;
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %u: %s\n", __LINE__, #x); } } while (0)
DerivedFeatures feature(const char* name, int hp = 12) {
    DerivedFeatures f{};
    std::strcpy(f.name, name); std::strcpy(f.type, "DruidCrow_Name");
    f.abilities = 1234; f.maxhp = f.hp = hp;
    return f;
}
int main() {
    CHECK(derived_named_for("Conan's Crow", "Conan"));
    CHECK(derived_named_for("Conan\xE2\x80\x99s Crow", "Conan"));
    CHECK(derived_named_for("Conan\xE7\x9A\x84\xE4\xB9\x8C\xE9\xB8\xA6", "Conan"));
    CHECK(!derived_named_for("Anna's Crow", "Ann"));
    CHECK(!derived_named_for("Conan", "Conan"));
    CHECK(!derived_named_for("Conan's ", "Conan"));
    CHECK(!derived_named_for("Crow", ""));
    DerivedHistory h;
    uint64_t ids[] = {100, 200};
    auto a = feature("Conan's Crow"), b = feature("Rex's Crow", 16);
    CHECK(!h.match(a, ids, 2).parent);
    h.remember(a, 100); h.remember(b, 200);
    CHECK(h.count == 2);
    CHECK(h.match(a, ids, 2).parent == 100);
    CHECK(h.match(b, ids, 2).parent == 200);
    a.hp = 1; a.shield = 30; a.maxhp = 14;
    CHECK(h.match(a, ids, 2).parent == 100); // Damage/buffs don't transfer ownership.
    CHECK(!h.match(a, ids + 1, 1).parent); // Old owner absent in this battle.
    auto missing_name = a; missing_name.name[0] = 0;
    CHECK(!h.match(missing_name, ids, 2).parent); // Two druids too similar: don't guess by HP.
    DerivedHistory one;
    one.remember(feature("Conan's Crow"), 100);
    CHECK(one.match(missing_name, ids, 2).parent == 0); // HP moved: below confidence threshold.
    missing_name.maxhp = 12;
    CHECK(one.match(missing_name, ids, 2).parent == 100);
    auto other = feature("Unknown's Crow");
    CHECK(!one.match(other, ids, 2).parent); // Explicit conflicting name beats stat similarity.
    other = a; std::strcpy(other.type, "DruidBear_Name");
    CHECK(!one.match(other, ids, 2).parent);
    other = {}; other.maxhp = 12;
    CHECK(!one.match(other, ids, 2).parent); // HP alone is never enough.
    h.remember(a, 100); unsigned n = h.count; h.remember(a, 100);
    CHECK(h.count == n); // Repeat observation updates instead of growing forever.
    h.remember(a, 200);
    CHECK(!h.match(a, ids, 2).parent); // Same name/types on both sides: ambiguous.
    for (unsigned i = 0; i < 200; ++i) h.remember(a, 1000 + i);
    CHECK(h.count == DerivedHistory::capacity);
    h.clear(); CHECK(h.count == 0 && !h.match(a, ids, 2).parent);
    // Roster order must not break ties or change winners.
    uint64_t reversed[] = {200,100};
    for (unsigned i = 0; i < 20; ++i) {
        DerivedHistory history;
        history.remember(feature("Conan's Crow", 12+i),100);
        history.remember(feature("Rex's Crow",12+i),200);
        CHECK(history.match(b, ids, 2).parent == history.match(b, reversed, 2).parent);
    }
    std::printf("test_derived: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
