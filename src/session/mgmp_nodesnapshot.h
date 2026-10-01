#pragma once

#include "mgmp_proto.h"
#include <utility>

namespace mgmp {

// TCP orders the state frames before ENTERNODE; moving this batch at that boundary prevents cross-node coalescing.
struct NodeSnapshot {
    static constexpr uint32_t kMaxCats = 64;
    CatDataMsg cats[kMaxCats] = {};
    uint32_t cat_count = 0;
    InventoryMsg inventory{};
    RunHistMsg history{};
    bool have_inventory = false;
    bool have_history = false;

    NodeSnapshot() = default;
    NodeSnapshot(const NodeSnapshot&) = delete;
    NodeSnapshot& operator=(const NodeSnapshot&) = delete;
    ~NodeSnapshot() { clear(); }

    bool empty() const { return !cat_count && !have_inventory && !have_history; }

    bool take(CatDataMsg& m) {
        uint32_t i = 0;
        while (i < cat_count && cats[i].id != m.id) ++i;
        if (i == kMaxCats) return false;
        if (i == cat_count) ++cat_count;
        free(cats[i].data);
        cats[i] = m;
        m = {};
        return true;
    }

    void take(InventoryMsg& m) {
        for (auto* p : inventory.data) free(p);
        inventory = m;
        m = {};
        have_inventory = true;
    }

    void take(RunHistMsg& m) {
        free(history.data);
        history = m;
        m = {};
        have_history = true;
    }

    void swap(NodeSnapshot& other) {
        for (uint32_t i = 0; i < kMaxCats; ++i) std::swap(cats[i], other.cats[i]);
        std::swap(cat_count, other.cat_count);
        std::swap(inventory, other.inventory);
        std::swap(history, other.history);
        std::swap(have_inventory, other.have_inventory);
        std::swap(have_history, other.have_history);
    }

    void clear() {
        for (uint32_t i = 0; i < cat_count; ++i) {
            free(cats[i].data);
            cats[i] = {};
        }
        cat_count = 0;
        for (auto* p : inventory.data) free(p);
        inventory = {};
        free(history.data);
        history = {};
        have_inventory = have_history = false;
    }
};

} // namespace mgmp
