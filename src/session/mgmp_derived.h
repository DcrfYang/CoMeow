#pragma once
#include <cstdint>
#include <cstring>

namespace mgmp {
// Pointer-free evidence, kept across battles in this run, reset with cat owners.
// Only authoritative parent/name observations may train the catalogue. Guesses
// never train it; otherwise a single wrong guess becomes permanent evidence.
struct DerivedFeatures {
    char name[384]{};
    char type[128]{}; // Authored localization key, independent of language.
    uint32_t abilities = 0;
    int32_t hp = 0, maxhp = 0, shield = 0;
};
struct DerivedRecord { DerivedFeatures features; uint64_t parent = 0; };
struct DerivedMatch { uint64_t parent = 0; int score = 0, margin = 0; };

inline bool derived_named_for(const char* name, const char* parent) {
    const size_t n = std::strlen(parent), len = std::strlen(name);
    if (!n || len <= n || std::strncmp(name, parent, n)) return false;
    const char* rest = name + n;
    // Possessive separators, not a substring match (Ann must not match Anna).
    return (!std::strncmp(rest, "'s ", 3) && rest[3]) ||
           (!std::strncmp(rest, "\xE2\x80\x99s ", 5) && rest[5]) ||
           (!std::strncmp(rest, "\xE7\x9A\x84", 3) && rest[3]); // Chinese 'de'
}

inline int derived_similarity(const DerivedFeatures& a, const DerivedFeatures& b) {
    const bool name = a.name[0] && b.name[0] && !std::strcmp(a.name, b.name);
    const bool type = a.type[0] && b.type[0] && !std::strcmp(a.type, b.type);
    const bool ability = a.abilities && b.abilities && a.abilities == b.abilities;
    if (a.name[0] && b.name[0] && !name) return 0;
    // HP alone cannot identify an owner. A conflicting authored type vetoes.
    if (a.type[0] && b.type[0] && !type) return 0;
    if (!name && !(type && ability)) return 0;
    int score = (name ? 65 : 0) + (type ? 30 : 0) + (ability ? 35 : 0);
    if (a.maxhp > 0 && b.maxhp > 0) {
        const int64_t delta = a.maxhp > b.maxhp ? int64_t(a.maxhp)-b.maxhp : int64_t(b.maxhp)-a.maxhp;
        const int64_t scale = a.maxhp > b.maxhp ? a.maxhp : b.maxhp;
        if (delta == 0) score += 15;
        else if (delta * 4 <= scale) score += 10;
    }
    return score;
}

struct DerivedHistory {
    static constexpr unsigned capacity = 128;
    DerivedRecord records[capacity]{};
    unsigned count = 0;
    void clear() { count = 0; }
    void remember(const DerivedFeatures& f, uint64_t parent) {
        if (!parent || (!f.name[0] && !f.type[0])) return;
        for (unsigned i = 0; i < count; ++i) {
            auto& r = records[i];
            if (r.parent == parent && !std::strcmp(r.features.name, f.name) &&
                !std::strcmp(r.features.type, f.type) && r.features.abilities == f.abilities &&
                r.features.maxhp == f.maxhp) { r.features = f; return; }
        }
        // Bounded, no order-dependent eviction that can erase a competing owner.
        if (count < capacity) records[count++] = {f, parent};
    }
    DerivedMatch match(const DerivedFeatures& f, const uint64_t* parents, unsigned n) const {
        DerivedMatch out{};
        int second = 0;
        for (unsigned p = 0; p < n; ++p) {
            bool duplicate = false;
            for (unsigned j = 0; j < p; ++j) duplicate |= parents[j] == parents[p];
            if (duplicate || !parents[p]) continue;
            int score = 0;
            for (unsigned i = 0; i < count; ++i) if (records[i].parent == parents[p]) {
                int s = derived_similarity(f, records[i].features);
                if (s > score) score = s;
            }
            if (score > out.score) { second = out.score; out.score = score; out.parent = parents[p]; }
            else if (score > second) second = score;
        }
        out.margin = out.score - second;
        if (out.score < 80 || out.margin < 20) out.parent = 0;
        return out;
    }
};
} // namespace mgmp
