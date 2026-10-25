// mgmp_souljar.cpp -- see mgmp_souljar.h.
#include "mgmp_souljar.h"

#include "mgmp_addresses.h"
#include "mgmp_catblob.h"
#include "mgmp_catbrief.h"
#include "mgmp_catsync.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_rng.h"     // rng_global_stream: the fence

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mgmp {
namespace {

// The call sites this module keys on (return addresses, RVAs of this game build) and the functions they must call -- checked once at startup.
constexpr uintptr_t kRa_SummonItem   = 0x7A3C51;   // SpawnAbility, `catdata item_aux_catid`: call 0x1400475C0 (the jar)
constexpr uintptr_t kRa_SummonLookup = 0x7A3C92;   // ... call 0x1400D7220 (CatDatabase::get(registry, aux))
constexpr uintptr_t kRa_DropInsert   = 0x712C8;    // DropSoulJarOnDeath's lambda 0x140070F90: call 0x1402E13F0 (Inventory::insert_item)
constexpr uintptr_t kFn_AbilityItem  = 0x475C0;
constexpr uintptr_t kFn_CatById      = 0xD7220;
constexpr uintptr_t kFn_InvInsert    = 0x2E13F0;
constexpr uintptr_t kAbility_Parent  = 0x708;      // 0x1400475C0 walks it to the root ability
constexpr uintptr_t kAbility_Char    = 0x10;       // ... whose +0x10 is the character it passes to the item finder 0x14010A460
constexpr uintptr_t kItem_Name       = 0x08;       // std::string, the item's gon name
constexpr uintptr_t kItem_Aux        = 0x4C;       // int32
constexpr const char* kFullJar       = "SoulJar_Full";

uintptr_t g_base = 0;
bool g_sites_ok = false;

// The summon's item, noted by souljar_note_item for the lookup that follows it in the same branch.
struct Pending { bool on = false; int32_t aux = -1; void* caster = nullptr; uint64_t caster_id = 0; };
Pending g_pending;

// The cats the room's jars hold, by (owner seat, aux). Game thread only.
struct Entry {
    bool     used = false;
    uint8_t  owner = 0;
    int32_t  aux = -1;
    uint8_t  status = 0;        // kSoulCatImage / kSoulCatMissing
    uint64_t hash = 0;
    uint32_t size = 0;
    uint64_t holder = 0;
    void*    cat = nullptr;     // built from the image (game heap); never freed -- a few per run
    char     name[kBriefNameMax] = {};
};
constexpr uint32_t kEntries = 32;
Entry g_e[kEntries];
uint32_t g_next_victim = 0;

uint64_t fnv(const uint8_t* p, uint32_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

bool in_room() { return net_active() && net_peer_count() >= 2; }

// THE SHARED STREAM IS PUT BACK around everything this module asks of the game (a lookup that loads a cat from the save, a cat built from an image,
// an image taken for the log): one peer does it and another does not, or at another moment, and nothing of it may move the stream the battle draws from.
struct StreamFence {
    uint64_t* s = nullptr;
    uint64_t saved[4] = {};
    bool on = false;
    StreamFence() { s = rng_global_stream(); on = s && mem_read(s, saved, sizeof(saved)); }
    ~StreamFence() { if (on) mem_write(s, saved, sizeof(saved)); }
};

// The seat a cat id names: a session clone (0x70000000 + seat<<24 + slot), a cat an event brought into the run (the host's), else unknown.
int seat_of(uint64_t id) {
    if (session_is_gained(id)) return 0;
    return session_cat_owner(id);
}
// Whose jar it is, in a room: the seat the cat's id names; a cat no seat names is the host's (the authority catsync gives such a cat).
uint8_t owner_seat(uint64_t id) { const int s = seat_of(id); return s >= 0 ? (uint8_t)s : 0; }

bool read_call_target(uintptr_t ret, uintptr_t want_fn) {
    uint8_t op = 0; int32_t rel = 0;
    if (!mem_read((const void*)(g_base + ret - 5), &op, 1) || !mem_read((const void*)(g_base + ret - 4), &rel, 4)) return false;
    return op == 0xE8 && g_base + ret + (intptr_t)rel == g_base + want_fn;
}

// The cat's name, as its own image has it ('?' when it cannot be read).
void cat_name(void* cat, char* out, size_t cap) {
    strncpy_s(out, cap, "?", _TRUNCATE);
    if (!cat) return;
    uint8_t* image = nullptr;
    const uint32_t n = serialize_cat(cat, &image);
    CatBrief b;
    if (n && image && catblob_parse(image, n, b) && b.name[0]) strncpy_s(out, cap, b.name, _TRUNCATE);
    free(image);
}

uint64_t cat_save_id(const void* cat) {
    uint64_t id = 0;
    if (cat) mem_read((const uint8_t*)cat + kCatData_SaveId, &id, sizeof(id));
    return id;
}

Entry* find(uint8_t owner, int32_t aux) {
    for (Entry& e : g_e) if (e.used && e.owner == owner && e.aux == aux) return &e;
    return nullptr;
}

// Keep what a jar's owner published (this peer's own jars go through here too, so the owner summons from the same bytes as everyone else).
void install(uint8_t owner, int32_t aux, uint8_t status, uint64_t holder, const uint8_t* image, uint32_t size, uint64_t hash, const char* how) {
    StreamFence fence;
    Entry* e = find(owner, aux);
    if (e && e->status == status && e->hash == hash && (status != kSoulCatImage || e->cat)) {
        log_line("SOULJAR", "%s: seat %u's jar aux %d (carried by cat %llx) -- the same cat as before ('%s', image hash %016llx), kept", how, (unsigned)owner, aux,
                 (unsigned long long)holder, e->name, (unsigned long long)hash);
        e->holder = holder;
        return;
    }
    if (!e) {
        for (Entry& x : g_e) if (!x.used) { e = &x; break; }
        if (!e) { e = &g_e[g_next_victim++ % kEntries]; log_line("SOULJAR", "the table of jar cats is full -- seat %u's aux %d gives up its place", (unsigned)e->owner, e->aux); }
    }
    Entry fresh;
    fresh.used = true; fresh.owner = owner; fresh.aux = aux; fresh.status = status; fresh.hash = hash; fresh.size = size; fresh.holder = holder;
    if (status == kSoulCatImage) {
        CatBrief b;
        if (catblob_parse(image, size, b) && b.name[0]) strncpy_s(fresh.name, sizeof(fresh.name), b.name, _TRUNCATE);
        else strncpy_s(fresh.name, sizeof(fresh.name), "?", _TRUNCATE);
        fresh.cat = catsync_cat_from_image(image, size, (uint64_t)(int64_t)aux);
        if (!fresh.cat) {
            log_line_lvl(LogLevel::Error, "SOULJAR", "!! %s: seat %u's jar aux %d holds '%s' (%u bytes, hash %016llx) but the image did not read into a cat here -- its summon will copy the caster",
                         how, (unsigned)owner, aux, fresh.name, size, (unsigned long long)hash);
            fresh.status = kSoulCatMissing;
        } else {
            log_line("SOULJAR", "%s: seat %u's jar aux %d (carried by cat %llx) holds '%s' -- %u bytes, hash %016llx, built here as the summon's source",
                     how, (unsigned)owner, aux, (unsigned long long)holder, fresh.name, size, (unsigned long long)hash);
        }
    } else {
        strncpy_s(fresh.name, sizeof(fresh.name), "(none)", _TRUNCATE);
        log_line_lvl(LogLevel::Warn, "SOULJAR", "%s: seat %u's jar aux %d (carried by cat %llx) holds a cat its owner's own save does not have -- every player's summon will copy the caster",
                     how, (unsigned)owner, aux, (unsigned long long)holder);
    }
    *e = fresh;
}

} // namespace

void souljar_set_base(uintptr_t base) {
    g_base = base;
    const bool item = read_call_target(kRa_SummonItem, kFn_AbilityItem);
    const bool look = read_call_target(kRa_SummonLookup, kFn_CatById);
    const bool drop = read_call_target(kRa_DropInsert, kFn_InvInsert);
    g_sites_ok = item && look && drop;
    if (g_sites_ok) log_line("SOULJAR", "the summon's two calls and the dropped jar's insert are where this build has them -- the Soul Jar fix is on");
    else log_line_lvl(LogLevel::Error, "SOULJAR", "!! the Soul Jar's call sites do not read as expected (summon item %s, summon lookup %s, drop insert %s) -- the fix is OFF and the game's own code runs",
                      item ? "ok" : "MOVED", look ? "ok" : "MOVED", drop ? "ok" : "MOVED");
}

void souljar_note_item(void* ability, void* item, uintptr_t ret) {
    if (!g_sites_ok || ret - g_base != kRa_SummonItem) return;
    g_pending = Pending{};
    if (!item) return;                                  // the game spawns its plain object (no cat) -- nothing to answer
    int32_t aux = -1;
    if (!mem_read((const uint8_t*)item + kItem_Aux, &aux, sizeof(aux)) || aux < 0) return;
    const uint8_t* root = (const uint8_t*)ability;
    for (int depth = 0; root && depth < 16; ++depth) {
        const uint8_t* up = nullptr;
        if (!mem_read(root + kAbility_Parent, &up, sizeof(up)) || !up) break;
        root = up;
    }
    const uint8_t* chr = nullptr;
    void* cat = nullptr;
    if (root) mem_read(root + kAbility_Char, &chr, sizeof(chr));
    if (chr) mem_read(chr + kChar_CatData, &cat, sizeof(cat));
    g_pending.on = true; g_pending.aux = aux; g_pending.caster = cat; g_pending.caster_id = cat_save_id(cat);
}

bool souljar_cat_lookup(void* registry, uint64_t id, uintptr_t ret, void* (*original)(void*, uint64_t), void*& out) {
    if (!g_sites_ok || ret - g_base != kRa_SummonLookup) return false;
    const Pending p = g_pending;
    g_pending = Pending{};
    StreamFence fence;
    const bool matched = p.on && (uint64_t)(int64_t)p.aux == id;
    const int seat = matched ? (int)owner_seat(p.caster_id) : -1;
    const bool room = in_room();
    if (room && seat >= 0) {
        const Entry* e = find((uint8_t)seat, p.aux);
        if (e && e->status == kSoulCatImage && e->cat) {
            out = e->cat;
            log_line("SOULJAR", "summon: cat %llx (seat %d) opens its jar (aux %d) -- the ghost is copied from '%s' as its owner published it (hash %016llx), the same on every player's game",
                     (unsigned long long)p.caster_id, seat, p.aux, e->name, (unsigned long long)e->hash);
            return true;
        }
        if (e && p.caster) {
            out = p.caster;
            log_line_lvl(LogLevel::Warn, "SOULJAR", "summon: cat %llx (seat %d) opens its jar (aux %d) -- its owner has no such cat, so the ghost is a copy of the caster (on every player's game)",
                         (unsigned long long)p.caster_id, seat, p.aux);
            return true;
        }
    }
    void* native = original(registry, id);
    if (native) {
        out = native;
        char nm[kBriefNameMax];
        cat_name(native, nm, sizeof(nm));
        if (room) log_line_lvl(LogLevel::Warn, "SOULJAR", "!! summon: cat %llx (seat %d) opens its jar (aux %d) and nobody published the cat it holds -- this game's own cat %llx '%s' is copied; another player's game may copy a different one%s",
                               (unsigned long long)p.caster_id, seat, (int)(int64_t)id, (unsigned long long)id, nm, matched ? "" : " (the caster was not noted)");
        else log_line("SOULJAR", "summon (alone): cat %llx opens its jar -- the ghost is a copy of this save's cat %llx '%s'", (unsigned long long)p.caster_id, (unsigned long long)id, nm);
        return true;
    }
    if (matched && p.caster) {
        out = p.caster;
        log_line_lvl(LogLevel::Error, "SOULJAR", "!! summon: cat %llx (seat %d) opens its jar (aux %d) and THIS SAVE HAS NO CAT %llx -- the game would have read a null pointer here (the crash); the ghost is a copy of the caster instead",
                     (unsigned long long)p.caster_id, seat, p.aux, (unsigned long long)id);
        return true;
    }
    log_line_lvl(LogLevel::Error, "SOULJAR", "!! summon: the jar's cat %llx is not in this save and the caster is not known -- the game's own (null) answer goes through", (unsigned long long)id);
    out = nullptr;
    return true;
}

bool souljar_drop_skip(void* item, int32_t loc, uintptr_t ret) {
    if (!g_sites_ok || ret - g_base != kRa_DropInsert || !item) return false;
    StreamFence fence;
    int32_t aux = -1;
    char name[48] = "?";
    mem_read((const uint8_t*)item + kItem_Aux, &aux, sizeof(aux));
    mem_read_std_string((const uint8_t*)item + kItem_Name, name, sizeof(name));
    const uint64_t dead = (uint64_t)(uint32_t)aux;
    char nm[kBriefNameMax];
    cat_name(catsync_registry_cat(dead), nm, sizeof(nm));
    const int seat = (int)owner_seat(dead);
    if (in_room() && seat != (int)catsync_my_seat()) {
        log_line("SOULJAR", "drop: cat %llx '%s' (seat %d) is gone and its jar became %s (aux %d) -- it goes to its owner's inventory, NOT this player's (seat %u)",
                 (unsigned long long)dead, nm, seat, name, aux, (unsigned)catsync_my_seat());
        return true;
    }
    log_line("SOULJAR", "drop: cat %llx '%s' (seat %d) is gone and its jar became %s (aux %d) -- put into this player's inventory (loc %d)%s",
             (unsigned long long)dead, nm, seat, name, aux, (int)loc, in_room() ? "" : ", alone");
    return false;
}

void souljar_on_party_built() {
    if (!g_sites_ok) return;
    StreamFence fence;
    const bool room = in_room();
    const uint8_t me = catsync_my_seat();
    uint64_t ids[64] = {};
    const uint32_t n = catsync_run_cat_list(ids, 64);
    unsigned jars = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (room && owner_seat(ids[i]) != me) continue;     // another player's cat: its owner publishes
        void* cat = catsync_registry_cat(ids[i]);
        if (!cat) continue;
        for (uint32_t k = 0; k < kCatItemCount; ++k) {
            const uint8_t* slot = (const uint8_t*)cat + kCatData_Items + k * kCatItemSize;
            int64_t uid = -1;
            char nm[48] = {};
            if (!mem_read(slot, &uid, sizeof(uid)) || uid == -1 || !mem_read_std_string(slot + kItem_Name, nm, sizeof(nm)) || strcmp(nm, kFullJar) != 0) continue;
            int32_t aux = -1;
            mem_read(slot + kItem_Aux, &aux, sizeof(aux));
            ++jars;
            char holder[kBriefNameMax];
            cat_name(cat, holder, sizeof(holder));
            if (aux < 0) { log_line("SOULJAR", "battle: cat %llx '%s' carries an empty %s (aux %d) -- nothing to publish", (unsigned long long)ids[i], holder, kFullJar, aux); continue; }
            void* held = catsync_registry_cat((uint64_t)(int64_t)aux);
            uint8_t* image = nullptr;
            const uint32_t size = held ? serialize_cat(held, &image) : 0;
            SoulCatMsg m;
            m.owner = room ? me : 0; m.aux = aux; m.holder = ids[i];
            if (size && image) { m.status = kSoulCatImage; m.size = size; m.hash = fnv(image, size); m.data = image; }
            else m.status = kSoulCatMissing;
            if (!room) {
                char hn[kBriefNameMax];
                cat_name(held, hn, sizeof(hn));
                log_line("SOULJAR", "battle (alone): cat %llx '%s' carries %s aux %d -> %s", (unsigned long long)ids[i], holder, kFullJar, aux,
                         held ? hn : "!! no such cat in this save (its summon will copy the caster)");
                free(image);
                continue;
            }
            log_line("SOULJAR", "battle: this player's cat %llx '%s' carries %s (slot %u) holding cat %d -- %s", (unsigned long long)ids[i], holder, kFullJar, k, aux,
                     m.status == kSoulCatImage ? "found in this save, its image goes to every player" : "!! this save has no such cat");
            install(m.owner, m.aux, m.status, m.holder, m.data, m.size, m.hash, "published");
            const bool sent = net_send_soulcat(m);
            if (!sent) log_line_lvl(LogLevel::Error, "SOULJAR", "!! the jar's cat (aux %d) could NOT be sent -- the other players' summon will not match", aux);
            free(image);
        }
    }
    if (jars) log_line("SOULJAR", "battle: %u filled soul jar(s) on %s cats", jars, room ? "this player's" : "the");
}

void souljar_on_message(uint8_t from, const SoulCatMsg& m) {
    // 0.1.83: a jar's cat is published by its owner only -- the seat the frame names must be the sender's (the membership list is in seat order)
    uint8_t ids[kMaxPeers] = {};
    if (net_peer_ids(ids, kMaxPeers)) {
        const uint8_t n = net_peer_count() > kMaxPeers ? kMaxPeers : net_peer_count();
        int seat = -1;
        for (uint8_t i = 0; i < n; ++i) if (ids[i] == from) seat = i;
        if (seat >= 0 && seat != (int)m.owner) {
            log_line_lvl(LogLevel::Warn, "SOULJAR", "!! peer %u (seat %d) sent the cat of seat %u's jar (aux %d) -- ignored: only a jar's owner publishes it", (unsigned)from, seat, (unsigned)m.owner, m.aux);
            return;
        }
    }
    install(m.owner, m.aux, m.status, m.holder, m.data, m.size, m.hash, "arrived");
}

void souljar_reset() {
    unsigned n = 0;
    for (Entry& e : g_e) if (e.used) { ++n; e = Entry{}; }       // the built cats stay on the game heap (a few per run), unreferenced
    g_pending = Pending{};
    if (n) log_line("SOULJAR", "a new session: %u jar cat(s) of the last one forgotten", n);
}

} // namespace mgmp
