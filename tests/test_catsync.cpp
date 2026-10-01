#include "mgmp_catsync.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_bytestream.h"
#include "mgmp_config.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_lockstep.h"
#include "mgmp_roster.h"
#include "mgmp_rng.h"
#include "mgmp_mem.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

using namespace mgmp;
namespace {
Config cfg;
unsigned checks = 0, failures = 0, post_loads = 0, reads = 0, destroyed_statuses = 0;
bool short_read = false, omit_hp = false;
int fail_reads = 0; bool replace_party_for_real = false;
bool battle_up = false, known_owners = false;
bool setup_ready = true, resume_owners = false;
bool notes_ready = false, split_flipped = false;
uint8_t peer_position = 0, peer_count = 2;
uint64_t rng[4] = {1, 2, 3, 4}, counter = 10;
unsigned char director[0x800]{}, registry[0x200]{};
const void* director_ptr = director;
uint64_t party[4] = {1, 2, 3, 4};
struct Entry { uint64_t padding[3]{}; void* cat = nullptr; };
struct IdVector { uint32_t capacity, count; uint64_t* data; };
struct SavedStatus { char* name; uint32_t id; uint8_t padding[0xB8 - 12]; };
struct StatusVector { SavedStatus* begin; SavedStatus* end; SavedStatus* capacity; };
struct CatImage {
    uint64_t seed;
    int32_t hp;
    uint32_t count;
    uint32_t statuses[4];
    int32_t stat;
    uint32_t equipment;
    uint32_t items[5]; // Optional records: an absent item does not overwrite the destination.
    uint64_t flags;    // CatData+0xBF8, serialized with the cat
};
static_assert(sizeof(IdVector) == 16 && offsetof(Entry, cat) == 0x18);
static_assert(sizeof(SavedStatus) == kCatStatusEffectSize && sizeof(CatImage) == 72);
std::map<uint64_t, Entry> entries;
std::vector<void*> allocated;
std::vector<uint64_t> published;
std::map<uint64_t, CatImage> published_images;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
template<class T> T get(const void* p, size_t off) { T v; std::memcpy(&v, (const char*)p + off, sizeof(v)); return v; }
template<class T> void put(void* p, size_t off, T v) { std::memcpy((char*)p + off, &v, sizeof(v)); }
uint64_t hash(const void* p, uint32_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t i = 0; i < n; ++i) { h ^= ((const uint8_t*)p)[i]; h *= 0x100000001b3ULL; }
    return h;
}
StatusVector& statuses(void* cat) { return *(StatusVector*)((uint8_t*)cat + kCatData_StatusEffects); }
uint32_t status_count(void* cat) {
    const auto& v = statuses(cat);
    return v.begin ? (uint32_t)(v.end - v.begin) : 0;
}
void __fastcall destroy_status_range(void* first, void* last) {
    for (auto* p = (SavedStatus*)first; p != last; ++p) {
        std::free(p->name);
        p->name = nullptr;
        ++destroyed_statuses;
    }
}
void append_status(void* cat, uint32_t id) {
    auto& v = statuses(cat);
    const uint32_t count = status_count(cat);
    if (v.end == v.capacity) {
        v.begin = (SavedStatus*)std::realloc(v.begin, (count + 4) * sizeof(SavedStatus));
        v.end = v.begin + count;
        v.capacity = v.begin + count + 4;
    }
    *v.end = {};
    v.end->id = id;
    v.end->name = (char*)std::malloc(32);
    std::snprintf(v.end->name, 32, "status-%u", id);
    ++v.end;
}
void* __fastcall allocate(size_t n) { void* p = std::calloc(1, n); allocated.push_back(p); return p; }
void* __fastcall construct(void* p) { ++rng[0]; return p; }
void __fastcall post_load(void*) { ++post_loads; ++rng[1]; }
Entry list_head;
void sync_list() {                      // rebuild the circular node list the registry walk reads
    Entry* prev = &list_head;
    for (auto& kv : entries) {
        Entry* e = &kv.second;
        e->padding[2] = kv.first;
        prev->padding[0] = (uint64_t)e; e->padding[1] = (uint64_t)prev; prev = e;
    }
    prev->padding[0] = (uint64_t)&list_head; list_head.padding[1] = (uint64_t)prev;
    void* head = &list_head; std::memcpy(registry + 0xE8 + 8, &head, sizeof(head));
}
void* __fastcall by_id(void*, uint64_t id) {
    sync_list();
    auto it = entries.find(id);
    return it == entries.end() ? nullptr : it->second.cat;
}
void* __fastcall register_cat(void* table, void* result, const uint64_t* id) {
    CHECK(table == registry + 0xE8);
    auto inserted = entries.emplace(*id, Entry{});
    put(result, 0, &inserted.first->second);
    put(result, 8, uint8_t(inserted.second));
    return result;
}
void __fastcall parents(void* table, uint64_t id, uint64_t a, uint64_t b) {
    CHECK(table == registry + 0x38);
    CHECK(entries.count(id) == 1 && a == UINT64_MAX && b == UINT64_MAX);
}
void __fastcall append(void* p, const uint64_t* id) {
    auto* v = (IdVector*)p;
    if (v->count == v->capacity) {
        v->capacity = v->capacity ? v->capacity * 2 : 2;
        v->data = (uint64_t*)std::realloc(v->data, v->capacity * sizeof(uint64_t));
    }
    v->data[v->count++] = *id;
}
void* __fastcall ofstream_ctor(void* p) { return p; }
void __fastcall bs_dtor(void* p) {
    if (get<uint32_t>(p, kBS_Mode) == 1) std::free(get<void*>(p, kBS_WriteBuf));
}
void __fastcall serialize(void* cat, void* bs, bool) {
    if (get<uint32_t>(bs, kBS_Mode) == 1) {
        CatImage image{};
        image.seed = get<uint64_t>(cat, 0);
        image.hp = get<int32_t>(cat, 0x7A8);
        image.stat = get<int32_t>(cat, 0x70C);
        image.equipment = get<uint32_t>(cat, 0x7D0);
        image.flags = get<uint64_t>(cat, kCatData_Flags);
        for (uint32_t i = 0; i < kCatItemCount; ++i) {
            const size_t off = kCatData_Items + i * kCatItemSize;
            if (get<uint64_t>(cat, off) != UINT64_MAX)
                image.items[i] = (uint32_t)get<uint64_t>(cat, off + 0x18);
        }
        image.count = status_count(cat);
        CHECK(image.count <= 4);
        for (uint32_t i = 0; i < image.count && i < 4; ++i) image.statuses[i] = statuses(cat).begin[i].id;
        void* data = std::malloc(sizeof(image));
        std::memcpy(data, &image, sizeof(image));
        put(bs, kBS_WriteBuf, data); put(bs, kBS_WriteLen, uint32_t(sizeof(image)));
    } else {
        ++reads;
        const uint32_t n = get<uint32_t>(bs, kBS_ReadLen);
        if (n == sizeof(CatImage)) {
            const auto& image = *get<const CatImage*>(bs, kBS_ReadBuf);
            put(cat, 0, image.seed);
            if (!omit_hp) put(cat, 0x7A8, image.hp);
            put(cat, 0x70C, image.stat);
            put(cat, 0x7D0, image.equipment);
            put(cat, kCatData_Flags, image.flags);
            for (uint32_t i = 0; i < kCatItemCount; ++i) {
                // RVA 0x22D470: absent record returns WITHOUT clearing the old item.
                if (!image.items[i]) continue;
                const size_t off = kCatData_Items + i * kCatItemSize;
                put(cat, off, ++counter);
                put(cat, off + 0x18, uint64_t(image.items[i]));
            }
            // RVA 0x22F301 appends each saved status without clearing the old vector.
            for (uint32_t i = 0; i < image.count && i < 4; ++i) append_status(cat, image.statuses[i]);
        }
        const bool fail_this = short_read || fail_reads > 0;
        if (fail_reads > 0) --fail_reads;
        put(bs, kBS_ReadPos, fail_this ? uint32_t(0) : n);
        ++rng[2];
    }
}
CatDataMsg message(uint64_t id, CatImage& image) {
    CatDataMsg m{}; m.id = id; m.size = sizeof(image);
    m.data = (uint8_t*)&image; m.hash = hash(&image, sizeof(image));
    return m;
}
void initialize() {
    std::strcpy(cfg.net_role, "client"); cfg.net_follow = true;
    std::memset(director, 0, sizeof(director));
    put(director, kDir_CatRegistry, registry + 0);
    put(director, kDir_CatIdCount, uint32_t(4));
    put(director, kDir_CatIdData, party + 0);
    for (auto id : party) {
        void* cat = allocate(0xC58); put(cat, kCatData_SaveId, id); put(cat, 0, id * 10);
        entries[id].cat = cat;
    }
    catsync_set_base(0); catsync_forget(); catsync_init();
}
void imports_and_membership() {
    const uint64_t id = 0x112233440000031dULL;
    CatImage image{0xfedcba9876543210ULL, 16, 1, {41}, 5, 10};
    auto m = message(id, image);
    const size_t before_allocations = allocated.size();
    CHECK(!catsync_apply_snapshot(m, "test", false));
    CHECK(allocated.size() == before_allocations && !entries.count(id));
    uint64_t before_rng[4]; std::memcpy(before_rng, rng, sizeof(rng));
    CHECK(catsync_apply_snapshot(m, "test", true));
    CHECK(std::memcmp(before_rng, rng, sizeof(rng)) == 0);
    CHECK(counter == id && post_loads == 1);
    CHECK(entries.count(id) == 1);
    void* cat = entries[id].cat;
    CHECK(get<uint64_t>(cat, kCatData_SaveId) == id);
    CHECK(get<uint64_t>(cat, 0) == image.seed);
    CHECK(get<int32_t>(cat, 0x7A8) == 16);
    CHECK(allocated.size() == before_allocations + 1);
    CHECK(status_count(cat) == 1);

    EnterNodeMsg node{}; node.seed0 = 100; node.familiar_count = 1; node.familiar_ids[0] = id;
    CHECK(catsync_apply_familiars(node));
    EnterNodeMsg observed{}; CHECK(catsync_read_familiars(observed));
    CHECK(observed.party_count == 4 && observed.party_ids[0] == party[0] && observed.party_ids[3] == party[3]);
    CHECK(observed.familiar_count == 1 && observed.familiar_ids[0] == id);
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 4);
    CHECK(std::memcmp(party, get<void*>(director, kDir_CatIdData), sizeof(party)) == 0);
    CHECK(catsync_apply_familiars(node));
    image.hp = 7; image.equipment = 20; m = message(id, image);
    CHECK(catsync_apply_snapshot(m, "changed image", true));
    CHECK(post_loads == 1 && allocated.size() == before_allocations + 1);
    CHECK(get<int32_t>(cat, 0x7A8) == 7);
    CHECK(get<uint32_t>(cat, 0x7D0) == 20);
    CHECK(status_count(cat) == 1 && statuses(cat).begin[0].id == 41);
    CHECK(std::memcmp(before_rng, rng, sizeof(rng)) == 0);

    const auto reads_before = reads;
    CHECK(catsync_apply_snapshot(m, "duplicate node batch", true));
    CHECK(reads == reads_before);
    put(cat, 0x7A8, int32_t(99));
    CHECK(catsync_apply_snapshot(m, "same hash but changed local data", true));
    CHECK(reads == reads_before + 1 && get<int32_t>(cat, 0x7A8) == 7);
    CHECK(status_count(cat) == 1);

    image.count = 0; image.statuses[0] = 0; m = message(id, image);
    CHECK(catsync_apply_snapshot(m, "empty saved statuses", true));
    CHECK(status_count(cat) == 0);
    CHECK(destroyed_statuses >= 3);

    image.hp = 12; m = message(id, image);
    omit_hp = true;
    CHECK(!catsync_apply_snapshot(m, "incomplete write", true));
    CHECK(get<int32_t>(cat, 0x7A8) == 7);
    omit_hp = false;
    CHECK(catsync_apply_snapshot(m, "retry after failed readback", true));
    CHECK(get<int32_t>(cat, 0x7A8) == 12);

    std::strcpy(cfg.net_role, "host");
    CHECK(catsync_publish("test", false, true));
    CHECK(published.size() == 5 && published.back() == id);
    std::strcpy(cfg.net_role, "client");
    node.familiar_count = 0;
    CHECK(catsync_apply_familiars(node));
    CHECK(catsync_read_familiars(observed) && observed.familiar_count == 0);
    CHECK(entries.count(id) == 1);
    CHECK(get<uint32_t>(director, kDir_CatIdCount) == 4);
}
void wide_names() {
    alignas(8) unsigned char s[32]{};
    char out[64]{};
    const wchar_t short_name[] = L"Conan";
    std::memcpy(s, short_name, sizeof(short_name));
    put(s, 16, uint64_t(5)); put(s, 24, uint64_t(7));
    CHECK(mem_read_std_wstring_utf8(s, out, sizeof(out)) && !std::strcmp(out, "Conan"));
    const wchar_t long_name[] = L"Conan\u7684\u4e4c\u9e26";
    put(s, 0, long_name); put(s, 16, uint64_t(8)); put(s, 24, uint64_t(15));
    CHECK(mem_read_std_wstring_utf8(s, out, sizeof(out)) &&
          !std::strcmp(out, "Conan\xE7\x9A\x84\xE4\xB9\x8C\xE9\xB8\xA6"));
    CHECK(!mem_read_std_wstring_utf8(s, out, 4) && !out[0]);
    put(s, 24, uint64_t(7));
    CHECK(!mem_read_std_wstring_utf8(s, out, sizeof(out)) && !out[0]);
    const wchar_t invalid[] = {0xD800, 0};
    put(s, 0, invalid); put(s, 16, uint64_t(1)); put(s, 24, uint64_t(15));
    CHECK(!mem_read_std_wstring_utf8(s, out, sizeof(out)) && !out[0]);
    put(s, 16, uint64_t(0));
    CHECK(mem_read_std_wstring_utf8(s, out, sizeof(out)) && !out[0]);
}

void optional_item_replacement() {
    void* cat = entries[1].cat;
    CatImage image{10, 10, 0, {}, 1, 0, {11, 22, 33, 44, 55}};
    auto m = message(1, image);
    CHECK(catsync_apply_snapshot(m, "all item slots populated", true));
    // RaccoonForm's removal of MonkFist + MonkStyleChanger caused a persistent
    // readback mismatch. Exercise absent/present transitions in every slot,
    // including identical repeated snapshots and re-equipping afterwards.
    for (uint32_t slot = 0; slot < kCatItemCount; ++slot) {
        image.items[slot] = 0;
        m = message(1, image);
        uint64_t before_rng[4]; std::memcpy(before_rng, rng, sizeof(rng));
        CHECK(catsync_apply_snapshot(m, "remove optional item", true));
        CHECK(get<uint64_t>(cat, kCatData_Items + slot * kCatItemSize) == UINT64_MAX);
        CHECK(std::memcmp(before_rng, rng, sizeof(rng)) == 0);
        const unsigned before_reads = reads;
        CHECK(catsync_apply_snapshot(m, "repeat empty item", true));
        CHECK(reads == before_reads);
    }
    image.items[3] = 77; image.items[4] = 88;
    m = message(1, image);
    CHECK(catsync_apply_snapshot(m, "equip both after removal", true));
    image.items[3] = image.items[4] = 0;
    m = message(1, image);
    CHECK(catsync_apply_snapshot(m, "remove both class items together", true));
}

void entry_replacement() {
    void* entry = allocate(0xC58);
    CatImage image{55, 9, 1, {73}, 8, 4};
    const auto before = destroyed_statuses;
    CHECK(mgmp::catsync_deserialize_into(entry, (uint8_t*)&image, sizeof(image)));
    CHECK(status_count(entry) == 1);
    CHECK(mgmp::catsync_deserialize_into(entry, (uint8_t*)&image, sizeof(image)));
    CHECK(status_count(entry) == 1 && destroyed_statuses == before + 1);
    image.count = 0; image.statuses[0] = 0;
    CHECK(mgmp::catsync_deserialize_into(entry, (uint8_t*)&image, sizeof(image)));
    CHECK(status_count(entry) == 0 && destroyed_statuses == before + 2);
}
void rejected_inputs() {
    EnterNodeMsg node{}; node.familiar_count = 1;
    node.familiar_ids[0] = 999; CHECK(!catsync_apply_familiars(node));
    node.familiar_ids[0] = 1; CHECK(!catsync_apply_familiars(node));
    node.familiar_ids[0] = 0; CHECK(!catsync_apply_familiars(node));
    node.familiar_count = 65; CHECK(!catsync_apply_familiars(node));
    CatImage image{55, 5, 0, {}, 1, 0};
    auto m = message(998, image); m.hash ^= 1;
    CHECK(!catsync_apply_snapshot(m, "test", true)); CHECK(!entries.count(998));
    m.hash ^= 1;
    std::strcpy(cfg.net_role, "host");
    CHECK(!catsync_apply_snapshot(m, "unauthorized creation", false)); CHECK(!entries.count(998));
    std::strcpy(cfg.net_role, "client");
    m.id = 997; short_read = true;
    CHECK(!catsync_apply_snapshot(m, "short read", true)); CHECK(!entries.count(997));
    short_read = false;
}
void local_upgrade_after_peer_push() {
    // Client owns cat 2, host owns 1/3/4. Host's update is held while the
    // client's upgrade screen still reports a battle, then drained on the map.
    catsync_forget();
    known_owners = true;
    CHECK(catsync_publish("seed shared save baseline", true));
    published.clear(); published_images.clear();
    CatImage remote{10, 11, 0, {}, 21, 31};
    const auto m = message(1, remote);
    battle_up = true;
    catsync_on_message(m);
    void* own = entries[2].cat;
    put(own, 0x70C, int32_t(88));
    put(own, 0x7D0, uint32_t(99));
    append_status(own, 901);
    CHECK(!catsync_publish("upgrade while remote cats held", false));
    CHECK(published.empty());
    catsync_apply_pending("returned to map");
    CHECK(get<int32_t>(entries[1].cat, 0x70C) == 21);
    CHECK(get<int32_t>(own, 0x70C) == 88 && status_count(own) == 1);
    CHECK(catsync_publish("map after draining peer update", true));
    CHECK(published.size() == 1 && published[0] == 2);
    CHECK(published_images[2].stat == 88 && published_images[2].equipment == 99);
    CHECK(published_images[2].count == 1 && published_images[2].statuses[0] == 901);
    published.clear();
    CHECK(catsync_publish("unchanged map tick", true));
    CHECK(published.empty());
    battle_up = known_owners = false;
}
void settlement_owner_filter() {
    // Model the live 4+4 shared lists at the instant the home scene calls the
    // native finalizer. The host owns 70..., so its result is party=70... and
    // no familiars; the peer copies lose the owned bit before native traversal.
    catsync_forget();
    peer_position = 0;
    std::strcpy(cfg.net_role, "host");
    catsync_init();
    uint64_t fam[4] = {0x71000001ull,0x71000002ull,0x71000003ull,0x71000004ull};
    uint64_t mir[4] = {0x70000001ull,0x70000002ull,0x70000003ull,0x70000004ull};
    uint64_t ids[8] = {0x70000001ull,0x70000002ull,0x70000003ull,0x70000004ull,
                       0x71000001ull,0x71000002ull,0x71000003ull,0x71000004ull};
    for (unsigned i = 0; i < 8; ++i) {
        void* cat = entries[ids[i]].cat;
        if (!cat) { cat = allocate(0xC58); entries[ids[i]].cat = cat; }
        put(cat, kCatData_SaveId, ids[i]); put(cat, 0xBF8, uint64_t(1ull | (1ull << 19)));
    }
    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdCap, uint32_t(4));
    put(director, kDir_CatIdData, (const uint64_t*)ids);
    put(director, kDir_MirrorCount, uint32_t(4)); put(director, kDir_MirrorCap, uint32_t(4));
    put(director, kDir_MirrorData, (const uint64_t*)mir);
    put(director, kDir_CatFamiliars, uint32_t(4)); put(director, kDir_CatFamiliars + 4, uint32_t(4));
    put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);
    resume_owners = true;
    CHECK(catsync_prepare_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 0);
    CHECK(std::memcmp(ids, get<void*>(director, kDir_CatIdData), sizeof(party)) == 0);
    for (unsigned i = 0; i < 4; ++i) {
        // Settlement filtering removes peer IDs from the native vectors.  It
        // deliberately leaves CatData flags untouched because their meaning
        // is broader than ownership and is not required for this boundary.
        CHECK(get<uint64_t>(entries[0x71000001ull+i].cat, 0xBF8) == (1ull | (1ull << 19)));
        CHECK(get<uint64_t>(entries[0x70000001ull+i].cat, 0xBF8) == (1ull | (1ull << 19)));
    }
    CHECK(!catsync_publish("after settlement", false, true));
    put(director, kDir_CatIdData, (const uint64_t*)party);
    put(director, kDir_CatFamiliars + 4, uint32_t(0)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)nullptr);
    put(director, kDir_MirrorData, (const uint64_t*)party);
    catsync_forget(); resume_owners = false; peer_position = 0;
}

void settlement_without_battle_split() {
    // 2026-09-28 live regression: a session restored from the journal walked an
    // event node into the home node with no battle ever starting, so the battle
    // split's table was empty on BOTH peers and the finalize was held forever
    // with the game frozen at the door. The settlement's ownership source is
    // the NOTE table (roster install / journal import); the split table is only
    // a cross-check that may be absent.
    catsync_forget();
    peer_position = 0;
    std::strcpy(cfg.net_role, "host");
    catsync_init();
    uint64_t fam[4] = {0x71000001ull,0x71000002ull,0x71000003ull,0x71000004ull};
    uint64_t mir[4] = {0x70000001ull,0x70000002ull,0x70000003ull,0x70000004ull};
    uint64_t ids[8] = {0x70000001ull,0x70000002ull,0x70000003ull,0x70000004ull,
                       0x71000001ull,0x71000002ull,0x71000003ull,0x71000004ull};
    for (unsigned i = 0; i < 8; ++i) {
        void* cat = entries[ids[i]].cat;
        if (!cat) { cat = allocate(0xC58); entries[ids[i]].cat = cat; }
        put(cat, kCatData_SaveId, ids[i]);
    }
    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdCap, uint32_t(4));
    put(director, kDir_CatIdData, (const uint64_t*)ids);
    put(director, kDir_MirrorCount, uint32_t(4)); put(director, kDir_MirrorCap, uint32_t(4));
    put(director, kDir_MirrorData, (const uint64_t*)mir);
    put(director, kDir_CatFamiliars, uint32_t(4)); put(director, kDir_CatFamiliars + 4, uint32_t(4));
    put(director, kDir_CatFamiliars + 8, (const uint64_t*)fam);

    // No battle: the split table is empty (is_mine fails for every 0x70/0x71
    // cat). Notes alone must settle the run -- this is the live fix.
    notes_ready = true;
    CHECK(catsync_prepare_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 0);
    CHECK(!catsync_publish("after settlement", false, true));
    catsync_forget();
    put(director, kDir_CatFamiliars + 4, uint32_t(4));

    // No notes either: the settlement must refuse rather than guess.
    notes_ready = false;
    CHECK(!catsync_prepare_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 4);

    // Notes present but the battle split DISAGREES: a real desync still refuses.
    notes_ready = true; resume_owners = true; split_flipped = true;
    CHECK(!catsync_prepare_settlement(director));
    CHECK(get<uint32_t>(director, kDir_CatFamiliars + 4) == 4);

    put(director, kDir_CatIdData, (const uint64_t*)party);
    put(director, kDir_CatFamiliars + 4, uint32_t(0)); put(director, kDir_CatFamiliars + 8, (const uint64_t*)nullptr);
    put(director, kDir_MirrorData, (const uint64_t*)party);
    catsync_forget(); notes_ready = resume_owners = split_flipped = false; peer_position = 0;
}

void in_session_next_run_replaces_stale_session_cats() {
    // 2026-09-28: a run that settles IN THIS SESSION leaves this peer's own
    // previous-run session clones registered -- a fresh process and a
    // checkpoint restore both rebuild the registry from the save, but the
    // in-session next chapter does not (observed live: 151 "already owned by
    // another cat" retries, the client's export never left, and the host's
    // chapter click stayed held). The second chapter barrier must REPLACE the
    // stale copy with the new selection, exactly like the receiving peer's
    // apply path does for a re-delivered identity.
    entries.clear();                             // drop every prior test's registry
    initialize();                                // client role; party = save cats 1..4
    SetupMsg first{};
    CHECK(catsync_prepare_party_setup(first, 1)); // run 1: the clones are minted
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(first.cats[i].id == 0x71000001ull + i);
        CHECK(entries.count(0x71000001ull + i) == 1);
        CHECK(get<uint64_t>(entries[0x71000001ull + i].cat, 0) == party[i] * 10);
    }
    for (auto& c : first.cats) free(c.data);
    // The run settles. The peer re-preps from its own save; the registry is
    // NOT rebuilt, so the four stale 0x71... clones are still registered while
    // the party now points at four DIFFERENT save cats.
    uint64_t reselected[4] = {5, 6, 7, 8};
    for (auto id : reselected) {
        void* cat = allocate(0xC58); put(cat, kCatData_SaveId, id); put(cat, 0, id * 10);
        entries[id].cat = cat;
    }
    put(director, kDir_CatIdCount, uint32_t(4));
    put(director, kDir_CatIdData, (const uint64_t*)reselected);
    SetupMsg second{};
    CHECK(catsync_prepare_party_setup(second, 1)); // failed before the fix
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(second.cats[i].id == 0x71000001ull + i);
        CHECK(second.cats[i].size == sizeof(CatImage));
        // The stale clone now carries the NEW selection's image, and the
        // exported image is the re-serialized replacement.
        CHECK(get<uint64_t>(entries[0x71000001ull + i].cat, 0) == reselected[i] * 10);
        CHECK(get<uint64_t>(entries[0x71000001ull + i].cat, kCatData_SaveId) ==
              0x71000001ull + i);
        const auto& image = *reinterpret_cast<const CatImage*>(second.cats[i].data);
        CHECK(image.seed == reselected[i] * 10);
        CHECK(second.cats[i].hash == hash(second.cats[i].data, second.cats[i].size));
        free(second.cats[i].data);
    }
    // Restore the standard fixture for the tests that follow.
    put(director, kDir_CatIdData, (const uint64_t*)party);
    for (auto id : reselected) entries.erase(id);
    catsync_forget();
}

constexpr uint64_t kHome = 3, kAway = 0x80001;
void depart_all(unsigned n) { for (unsigned i = 0; i < n; ++i) put(entries[party[i]].cat, kCatData_Flags, uint64_t(kAway)); }
void settle_clone(uint64_t id, int32_t stat) {          // what the native settlement + the run did to a clone
    put(entries[id].cat, 0x70C, stat); put(entries[id].cat, kCatData_Flags, uint64_t(kHome));
}
uint64_t flags_of(uint64_t id) { return get<uint64_t>(entries[id].cat, kCatData_Flags); }
void free_msg_cats(SetupMsg& m) { for (auto& c : m.cats) { free(c.data); c.data = nullptr; } }

void clones_return_to_their_originals() {
    // F1 (2026-10-01): the game flags the ORIGINALS on adventure at departure and settles only the clones, so
    // after a run the progress sat on the clones and the originals stayed out forever. The merge writes each
    // settled clone back over the cat it was made from and retires it.
    entries.clear(); peer_position = 1; peer_count = 2; initialize(); depart_all(4);
    SetupMsg first{};
    CHECK(catsync_prepare_party_setup(first, 1)); free_msg_cats(first);
    for (unsigned i = 0; i < 4; ++i) CHECK(flags_of(0x71000001ull + i) == kAway);   // the clones left with the originals' flags
    CHECK(catsync_merge_session_cats("test") == 0);                                  // still out on the run: nothing to merge
    for (unsigned i = 0; i < 4; ++i) settle_clone(0x71000001ull + i, 70 + (int32_t)i);
    CHECK(catsync_merge_session_cats("test") == 4);
    { const uint64_t* now = get<const uint64_t*>(director, kDir_CatIdData);       // the party list names the originals again
      CHECK(get<uint32_t>(director, kDir_CatIdCount) == 4);
      for (unsigned i = 0; i < 4; ++i) CHECK(now[i] == party[i]); }
    for (unsigned i = 0; i < 4; ++i) {
        void* orig = entries[party[i]].cat;
        CHECK(get<int32_t>(orig, 0x70C) == 70 + (int32_t)i);                         // the run's progress is on the original
        CHECK(flags_of(party[i]) == kHome);                                          // at home again, no longer "on adventure"
        CHECK(get<uint64_t>(orig, kCatData_SaveId) == party[i]);                     // identity kept
        CHECK(get<uint64_t>(orig, 0) == party[i] * 10);                              // same cat: same seed
        CHECK(flags_of(0x71000001ull + i) == 0);                                     // the clone is retired
    }
    CHECK(catsync_merge_session_cats("again") == 0);                                 // idempotent
    entries.clear(); peer_position = 0;
}

void legacy_clones_are_merged_before_they_are_overwritten() {
    // The old behaviour left the clones as LIVE cats at home. Run 2 picks clones 3 and 4 only; the fixed slot ids
    // 71000001/2 are live cats that used to be overwritten (data lost). Now they are merged back first.
    entries.clear(); peer_position = 1; peer_count = 2; initialize(); depart_all(4);
    SetupMsg first{};
    CHECK(catsync_prepare_party_setup(first, 1)); free_msg_cats(first);
    for (unsigned i = 0; i < 4; ++i) settle_clone(0x71000001ull + i, 40 + (int32_t)i);   // legacy: live clones, originals still out
    uint64_t picked[2] = {0x71000003ull, 0x71000004ull};
    put(director, kDir_CatIdCount, uint32_t(2)); put(director, kDir_CatIdData, (const uint64_t*)picked);
    for (auto id : picked) put(entries[id].cat, kCatData_Flags, uint64_t(kHome | 0x80000));   // departure flags them
    SetupMsg second{};
    CHECK(catsync_prepare_party_setup(second, 1)); free_msg_cats(second);
    for (unsigned i = 0; i < 2; ++i) {                                                   // 1 and 2 were NOT lost
        CHECK(get<int32_t>(entries[party[i]].cat, 0x70C) == 40 + (int32_t)i);
        CHECK(flags_of(party[i]) == kHome);
    }
    CHECK(get<uint64_t>(entries[0x71000001ull].cat, 0) == party[2] * 10);                // the slots now carry cats 3 and 4
    CHECK(get<uint64_t>(entries[0x71000002ull].cat, 0) == party[3] * 10);
    put(director, kDir_CatIdCount, uint32_t(4)); put(director, kDir_CatIdData, (const uint64_t*)party);
    entries.clear(); peer_position = 0; catsync_forget();
}

// The cat (if any) that is not one of the fixture's ids and carries `stat` -- what an adopted clone looks like.
bool adopted_with_stat(int32_t stat) {
    for (auto& kv : entries) {
        if (kv.first < 100 || (kv.first >= 0x70000000ull && kv.first < 0x80000000ull) || !kv.second.cat) continue;
        if (get<int32_t>(kv.second.cat, 0x70C) == stat && get<uint64_t>(kv.second.cat, kCatData_SaveId) == kv.first) return true;
    }
    return false;
}

void clones_without_an_original_and_the_id_counter() {
    // The game numbers new cats from a counter. Registering a session id used to raise it into the session range, so a
    // later recruit got an id the mod reads as a clone (and that fails the settlement's ownership check).
    entries.clear(); peer_position = 1; peer_count = 2; initialize(); counter = 0x300;
    depart_all(4);
    SetupMsg m{}; CHECK(catsync_prepare_party_setup(m, 1)); free_msg_cats(m);
    CHECK(counter == 0x300);                                              // the clones did not drag it up
    // an older build had already done it: the counter sits inside the session range and is put back
    counter = 0x71000009ull;
    { void* c = allocate(0xC58); put(c, kCatData_SaveId, uint64_t(0x2f0)); put(c, 0, uint64_t(777)); entries[0x2f0].cat = c; }
    sync_list();
    catsync_merge_session_cats("t");
    CHECK(counter == 0x2f0);

    // a clone made from a clone of an older build (slot 8, still out on adventure): the origin is that cat, not a new one
    entries.clear(); initialize(); counter = 0x300; depart_all(4);
    { void* old = allocate(0xC58); put(old, kCatData_SaveId, uint64_t(0x71000008ull)); put(old, 0, party[0] * 10);
      put(old, kCatData_Flags, uint64_t(kAway)); entries[0x71000008ull].cat = old; }
    SetupMsg m2{}; CHECK(catsync_prepare_party_setup(m2, 1)); free_msg_cats(m2);
    entries[party[0]].cat = nullptr; entries.erase(party[0]);              // the true original is nowhere
    for (unsigned i = 0; i < 4; ++i) settle_clone(0x71000001ull + i, 50 + (int32_t)i);
    CHECK(catsync_merge_session_cats("t") == 4);
    CHECK(get<int32_t>(entries[0x71000008ull].cat, 0x70C) == 50 && flags_of(0x71000008ull) == kHome);   // revived with the run's progress
    CHECK(flags_of(0x71000001ull) == 0 && counter == 0x300 + 0);        // no new cat was needed for it
    // a leftover of an older build that is itself settled and has no original is simply the cat it is
    { void* lone = allocate(0xC58); put(lone, kCatData_SaveId, uint64_t(0x71000009ull)); put(lone, 0, uint64_t(31337)); put(lone, kCatData_Flags, uint64_t(kHome)); entries[0x71000009ull].cat = lone; }
    CHECK(catsync_merge_session_cats("t") == 0 && flags_of(0x71000009ull) == kHome);
    entries.clear(); peer_position = 0; catsync_forget();
}

void copies_at_home_do_not_make_the_original_ambiguous() {
    // 2026-10-01 live: every adopted clone is an ordinary cat with the SAME seed as the cat it came from, so after one
    // adoption the next merge saw two ordinary cats with that seed, called it ambiguous, and adopted again -- the
    // saves filled with copies. Only a cat that is OUT on adventure can be the original.
    entries.clear(); peer_position = 1; peer_count = 2; initialize(); counter = 0x300; depart_all(4);
    SetupMsg m{}; CHECK(catsync_prepare_party_setup(m, 1)); free_msg_cats(m);
    for (unsigned i = 0; i < 4; ++i) settle_clone(0x71000001ull + i, 60 + (int32_t)i);
    for (uint64_t id : {0x2f0ull, 0x2f1ull}) {                                        // two copies at home with cat 1's seed
        void* c = allocate(0xC58); put(c, kCatData_SaveId, id); put(c, 0, party[0] * 10); put(c, kCatData_Flags, uint64_t(kHome)); entries[id].cat = c;
    }
    sync_list();
    CHECK(catsync_merge_session_cats("t") == 4);
    CHECK(get<int32_t>(entries[party[0]].cat, 0x70C) == 60 && flags_of(party[0]) == kHome);   // written to the real original
    CHECK(!adopted_with_stat(60));                                                            // and nothing new was made
    entries.clear(); peer_position = 0; catsync_forget();
}

void stuck_originals_are_released() {
    // Cats the old behaviour left "on adventure" for good come home when no run of this player is in progress;
    // the run's own party (just departed) and the session range are never touched.
    entries.clear(); peer_position = 1; peer_count = 2; initialize(); depart_all(4);
    for (uint64_t id : {50ull, 51ull, 52ull}) {
        void* c = allocate(0xC58); put(c, kCatData_SaveId, id); put(c, 0, id * 10);
        put(c, kCatData_Flags, uint64_t(id == 52 ? kHome : kAway)); entries[id].cat = c;
    }
    { void* sc = allocate(0xC58); put(sc, kCatData_SaveId, uint64_t(0x70000001ull)); put(sc, 0, uint64_t(777)); put(sc, kCatData_Flags, uint64_t(kAway)); entries[0x70000001ull].cat = sc; }
    sync_list();
    CHECK(catsync_release_stuck_cats("t") == 2);
    CHECK(flags_of(50) == 1 && flags_of(51) == 1 && flags_of(52) == kHome);          // 50 and 51 are home again
    for (unsigned i = 0; i < 4; ++i) CHECK(flags_of(party[i]) == kAway);             // the run's own party stays out
    CHECK(flags_of(0x70000001ull) == kAway);                                         // a session cat is not this function's business
    CHECK(catsync_release_stuck_cats("again") == 0);
    entries.clear(); peer_position = 0; catsync_forget();
}

void merge_refusals_and_rollback() {
    auto fresh = [] {
        entries.clear(); peer_position = 1; peer_count = 2; initialize(); counter = 0x300; depart_all(4);
        SetupMsg m{}; CHECK(catsync_prepare_party_setup(m, 1)); free_msg_cats(m);
        for (unsigned i = 0; i < 4; ++i) settle_clone(0x71000001ull + i, 90 + (int32_t)i);
    };
    // the original is not on adventure: it may have been played on its own -- never write over it. The clone is kept
    // as a NEW cat of its own instead (its slot is about to be re-used, so leaving it would lose it).
    fresh(); put(entries[party[0]].cat, kCatData_Flags, uint64_t(kHome));
    CHECK(catsync_merge_session_cats("t") == 4 && flags_of(0x71000001ull) == 0 && get<int32_t>(entries[party[0]].cat, 0x70C) != 90);
    CHECK(adopted_with_stat(90) && flags_of(party[0]) == kHome);
    // the game put the flags of the retired clone back (live: the departure box starts from the party/mirror lists):
    // the next merge must not keep the same clone as yet another new cat
    { size_t before = entries.size(); put(entries[0x71000001ull].cat, kCatData_Flags, uint64_t(kHome));
      CHECK(catsync_merge_session_cats("again") == 0 && flags_of(0x71000001ull) == 0 && entries.size() == before); }
    // two ordinary cats with the clone's seed: ambiguous, so no match -- the clone becomes a new cat
    fresh(); { void* twin = allocate(0xC58); put(twin, kCatData_SaveId, uint64_t(99)); put(twin, 0, party[1] * 10); put(twin, kCatData_Flags, uint64_t(kAway)); entries[99].cat = twin; }
    CHECK(catsync_merge_session_cats("t") == 4 && flags_of(0x71000002ull) == 0 && adopted_with_stat(91));
    // no original at all: kept as a new cat, with every bit of the progress it carries
    fresh(); entries[party[2]].cat = nullptr; entries.erase(party[2]);
    CHECK(catsync_merge_session_cats("t") == 4 && flags_of(0x71000003ull) == 0 && adopted_with_stat(92));
    // a clone still out on a run
    fresh(); put(entries[0x71000004ull].cat, kCatData_Flags, uint64_t(kAway));
    CHECK(catsync_merge_session_cats("t") == 3 && flags_of(0x71000004ull) == kAway);
    // another player's clones are never touched
    fresh(); { void* other = allocate(0xC58); put(other, kCatData_SaveId, uint64_t(0x70000001ull)); put(other, 0, party[0] * 10); put(other, kCatData_Flags, uint64_t(kHome)); entries[0x70000001ull].cat = other; }
    CHECK(catsync_merge_session_cats("t") == 4 && flags_of(0x70000001ull) == kHome);
    // a failed write puts the original back as it was and leaves the clone alone
    fresh(); const int32_t before = get<int32_t>(entries[party[0]].cat, 0x70C); fail_reads = 1;
    CHECK(catsync_merge_session_cats("t") == 3);
    CHECK(get<int32_t>(entries[party[0]].cat, 0x70C) == before && flags_of(party[0]) == kAway && flags_of(0x71000001ull) == kHome);
    fail_reads = 0;
    entries.clear(); peer_position = 0; catsync_forget();
}

void variable_exports_and_settlement() {
 peer_count=4;entries.clear();initialize();
 // Each local count, each owner, cloning over previous-run registry entries.
 for(unsigned p=0;p<4;++p)for(unsigned n=1;n<=4;++n){
  put(director,kDir_CatIdCount,(uint32_t)n);SetupMsg m{};
  CHECK(catsync_prepare_party_setup(m,(uint8_t)p));CHECK(m.count==n);
  for(unsigned j=0;j<n;++j){CHECK(m.cats[j].id==0x70000001ull+((uint64_t)p<<24)+j);free(m.cats[j].data);}
 }
 // Invalid partial export must release every already serialized cat.
 uint64_t badids[]={1,1};put(director,kDir_CatIdData,badids+0);put(director,kDir_CatIdCount,uint32_t(2));
 SetupMsg bad{};CHECK(!catsync_export_party_setup(bad)&&bad.count==0);for(auto& c:bad.cats)CHECK(!c.data);
 put(director,kDir_CatIdCount,uint32_t(0));CHECK(!catsync_export_party_setup(bad));
 uint64_t rng_before[4];memcpy(rng_before,rng,32);
 for(unsigned n=2;n<=4;++n)for(unsigned code=0;code<(1u<<(2*n));++code){
  unsigned sizes[4]{},x=code;for(unsigned p=0;p<n;++p){sizes[p]=1+x%4;x/=4;}
  peer_count=(uint8_t)n;
  for(unsigned owner=0;owner<n;++owner)for(unsigned local=0;local<2;++local){
   catsync_forget();setup_ready=notes_ready=resume_owners=true;peer_position=(uint8_t)owner;
   uint64_t pa[4]{},mi[4]{},fa[16]{};uint32_t pn=0,fn=0;
   unsigned displayed=local?owner:0;
   for(unsigned p=0;p<n;++p)for(unsigned j=0;j<sizes[p];++j){uint64_t id=0x70000001ull+((uint64_t)p<<24)+j;if(p==displayed)pa[pn++]=id;else fa[fn++]=id;}
   memcpy(mi,pa,32);
   put(director,kDir_CatIdCap,uint32_t(4));put(director,kDir_CatIdCount,pn);put(director,kDir_CatIdData,pa+0);
   put(director,kDir_MirrorCap,uint32_t(4));put(director,kDir_MirrorCount,pn);put(director,kDir_MirrorData,mi+0);
   put(director,kDir_CatFamiliars,uint32_t(16));put(director,kDir_CatFamiliars+4,fn);put(director,kDir_CatFamiliars+8,fa+0);
   SetupMsg m{};CHECK(catsync_export_resume(m,(uint8_t)owner));CHECK(m.count==sizes[owner]);
   for(unsigned j=0;j<m.count;++j){CHECK(m.cats[j].id==0x70000001ull+((uint64_t)owner<<24)+j);free(m.cats[j].data);}
   if(code==0 && local==0){
    // Before any battle there are owner notes, but no battle split yet.
    resume_owners=false;published.clear();
    CHECK(catsync_publish("first map owner-only publish",false,true));
    CHECK(published.size()==sizes[owner]);for(auto id:published)CHECK(session_cat_owner(id)==(int)owner);
    CatImage forged{123456,5};auto own_message=message(0x70000001ull+((uint64_t)owner<<24),forged);
    const unsigned before_reads=reads;
    CHECK(catsync_apply_snapshot(own_message,"keep own pre-battle cat"));CHECK(reads==before_reads);
    resume_owners=true;
   }
   CHECK(catsync_prepare_settlement(director));
   CHECK(get<uint32_t>(director,kDir_CatIdCount)==sizes[owner]);
   CHECK(get<uint32_t>(director,kDir_MirrorCount)==sizes[owner]);
   CHECK(get<uint32_t>(director,kDir_CatFamiliars+4)==0);
   for(unsigned j=0;j<sizes[owner];++j)CHECK(pa[j]==0x70000001ull+((uint64_t)owner<<24)+j&&mi[j]==pa[j]);
   CHECK(!catsync_prepare_settlement(director)); // cannot settle twice
  }
 }
 CHECK(!memcmp(rng_before,rng,32));
 peer_count=2;peer_position=0;notes_ready=resume_owners=false;catsync_forget();
 put(director,kDir_CatIdData,party+0);put(director,kDir_CatIdCount,uint32_t(4));
 put(director,kDir_MirrorData,party+0);put(director,kDir_MirrorCount,uint32_t(4));
 put(director,kDir_CatFamiliars+4,uint32_t(0));put(director,kDir_CatFamiliars+8,(uint64_t*)nullptr);
}

void resume_owned_snapshots() {
    catsync_forget();
    peer_position = 1;
    auto* vec = (IdVector*)(director + kDir_CatFamiliars);
    const IdVector saved_vec = *vec;
    uint64_t old_party[4]; memcpy(old_party, party, sizeof(party));
    uint64_t companions[4] = {};
    for (int i = 0; i < 8; ++i) {
        uint64_t id = 0x70000001ull + (uint64_t(i / 4) << 24) + (i % 4);
        void* cat = allocate(0xC58); entries[id].cat = cat;
        put(cat, kCatData_SaveId, id); put(cat, 0x7A8, int32_t(10 + i));
        // Deliberately use CLIENT party, HOST familiars, just like saved slot2.
        if (i < 4) companions[i] = id; else party[i - 4] = id;
    }
    *vec = {4, 4, companions};
    SetupMsg own{}, host{};
    uint64_t old_rng[4]; memcpy(old_rng, rng, sizeof(rng));
    CHECK(catsync_export_resume(own, 1));
    CHECK(catsync_export_resume(host, 0));
    CHECK(memcmp(old_rng, rng, sizeof(rng)) == 0);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(own.cats[i].id == 0x71000001ull + i);
        CHECK(host.cats[i].id == 0x70000001ull + i);
        CatImage c{}; memcpy(&c, own.cats[i].data, sizeof(c));
        CHECK(c.hp == 14 + i);
    }
    CHECK(party[0] == 0x71000001ull && companions[0] == 0x70000001ull);
    SetupMsg bad{};
    companions[0] = party[0]; CHECK(!catsync_export_resume(bad, 1) && bad.count == 0);
    companions[0] = 1; CHECK(!catsync_export_resume(bad, 1) && bad.count == 0);
    companions[0] = 0x70000001ull;
    vec->count = 3; CHECK(catsync_export_resume(bad, 1));
    for(auto& c:bad.cats) free(c.data); bad=SetupMsg{}; vec->count = 4;
    CHECK(!catsync_export_resume(bad, 2));

    published.clear(); setup_ready = false;
    CHECK(!catsync_publish("before resume handshake", false, true));
    for (int i = 0; i < 8; ++i) catsync_map_tick();
    CHECK(published.empty());
    CatImage incoming{99, 44}; auto remote = message(0x70000001ull, incoming);
    unsigned old_reads = reads;
    catsync_on_message(remote); catsync_apply_pending("before handshake");
    CHECK(reads == old_reads);
    // SETUP explicitly applies the peer snapshot, while normal CATDATA is gated.
    CHECK(catsync_apply_snapshot(remote, "resume setup"));
    CHECK(get<int32_t>(entries[remote.id].cat, 0x7A8) == 44);
    catsync_setup_sent(own);
    setup_ready = resume_owners = true;
    put(entries[own.cats[0].id].cat, 0x70C, int32_t(77));
    CHECK(catsync_publish("edit after resume", false));
    CHECK(published.size() == 1 && published[0] == own.cats[0].id);
    CHECK(published_images[own.cats[0].id].stat == 77);
    for (auto& c : own.cats) free(c.data);
    for (auto& c : host.cats) free(c.data);
    *vec = saved_vec; memcpy(party, old_party, sizeof(party));
    resume_owners = false;
    catsync_forget();
}

// 2026-09-29: the per-cat digest heals a drifted copy from its OWNER, is bounded, and
// never lets a peer push a cat it does not own. Ownership in this harness: with
// known_owners, cat 2 "is mine" on whichever peer asks; with nothing known, the host.
std::vector<CatDigestMsg> digests;
std::vector<uint8_t> published_to;
bool digest_cycle_ran() {
    const size_t n = digests.size();
    for (int i = 0; i < 200 && digests.size() == n; ++i) catsync_map_tick();
    return digests.size() > n;
}
CatDigestMsg altered(const CatDigestMsg& d, uint64_t id, uint64_t hash) {
    CatDigestMsg out = d;
    for (uint32_t i = 0; i < out.count; ++i) if (out.ids[i] == id) out.hashes[i] = hash;
    return out;
}
void digest_heals_from_owner() {
    const uint64_t ids[4] = {1, 2, 3, 4};
    std::memcpy(party, ids, sizeof(party));
    initialize();
    known_owners = true; notes_ready = false; battle_up = false; setup_ready = true;
    peer_position = 1; peer_count = 2;
    CHECK(catsync_publish("seed shared save baseline", true));
    published.clear(); published_to.clear(); digests.clear();

    // A digest goes out about once a second, listing every cat with its image hash.
    CHECK(digest_cycle_ran());
    const CatDigestMsg mine = digests.back();
    CHECK(mine.count == 4);
    for (uint32_t i = 0; i < mine.count; ++i) CHECK(mine.ids[i] == ids[i] && mine.sizes[i] == sizeof(CatImage));
    CHECK(published.empty());

    // The host holds a different copy of cat 2, which is THIS client's: one digest is
    // not enough (a push may be in flight), the second identical one triggers a resend.
    const CatDigestMsg host_stale = altered(mine, 2, 0x1234);
    catsync_on_digest(0, host_stale); CHECK(digest_cycle_ran());
    CHECK(published.empty());
    catsync_on_digest(0, host_stale); CHECK(digest_cycle_ran());
    CHECK(published.size() == 1 && published[0] == 2);
    // A client reaches only the host, so it heals by broadcast (the host relays).
    CHECK(published_to.empty());
    CHECK(published_images[2].seed == 20);

    // Bounded: four quick resends, then one per 16 digests, never a stop.
    published.clear();
    for (int k = 0; k < 18; ++k) { catsync_on_digest(0, host_stale); CHECK(digest_cycle_ran()); }
    CHECK(published.size() == 4);   // attempts 2, 3, 4 and the one at the 16th digest

    // Agreement drops the entry; a later difference starts from one again.
    published.clear();
    catsync_on_digest(0, mine); CHECK(digest_cycle_ran());
    catsync_on_digest(0, host_stale); CHECK(digest_cycle_ran());
    CHECK(published.empty());
    catsync_on_digest(0, host_stale); CHECK(digest_cycle_ran());
    CHECK(published.size() == 1);

    // Cat 1 is the host's: this client only reports the difference, however long it lasts.
    published.clear();
    const CatDigestMsg other = altered(mine, 1, 0x5678);
    for (int k = 0; k < 6; ++k) { catsync_on_digest(0, other); CHECK(digest_cycle_ran()); }
    CHECK(published.empty());

    // Membership differences are reported, never healed here.
    CatDigestMsg fewer = mine; fewer.count = 3;
    catsync_on_digest(0, fewer); CHECK(digest_cycle_ran());
    catsync_on_digest(0, fewer); CHECK(digest_cycle_ran());
    CHECK(published.empty());

    // Its own digest echoed back and a digest from an impossible position are ignored.
    catsync_on_digest(1, host_stale); catsync_on_digest(200, host_stale);
    CHECK(digest_cycle_ran()); CHECK(published.empty());

    // HOST, three players, owners unknown: the host is the authority for every cat and
    // answers the drifted peer DIRECTLY, not everyone.
    std::strcpy(cfg.net_role, "host");
    catsync_forget();
    known_owners = false; peer_position = 0; peer_count = 3;
    CHECK(catsync_publish("host baseline", true));
    published.clear(); published_to.clear(); digests.clear();
    CHECK(digest_cycle_ran());
    const CatDigestMsg host_mine = digests.back();
    const CatDigestMsg client2 = altered(host_mine, 3, 0x9ABC);
    catsync_on_digest(2, client2); CHECK(digest_cycle_ran());
    catsync_on_digest(2, client2); CHECK(digest_cycle_ran());
    CHECK(published.size() == 1 && published[0] == 3);
    CHECK(published_to.size() == 1 && published_to[0] == 2);

    // CLIENT 1 of three, owners unknown: the host decides, so client 1 never sends.
    std::strcpy(cfg.net_role, "client");
    catsync_forget();
    peer_position = 1;
    CHECK(catsync_publish("client baseline", true));
    published.clear(); published_to.clear();
    for (int k = 0; k < 4; ++k) { catsync_on_digest(2, client2); CHECK(digest_cycle_ran()); }
    CHECK(published.empty());

    // Held cats mean this peer is behind: no digest is sent and nothing is compared.
    battle_up = true;
    CatImage incoming{77, 5};
    catsync_on_message(message(4, incoming));
    battle_up = false;
    CHECK(digest_cycle_ran());   // the drain happens on the map tick, before the digest
    CHECK(get<int32_t>(entries[4].cat, 0x7A8) == 5);

    peer_position = 0; peer_count = 2;
    catsync_forget();
}

}
namespace mgmp {
const Config& config() { return cfg; }
bool net_active() { return true; }
bool setup_runtime_ready() { return setup_ready; }
uint8_t net_peer_pos() { return peer_position; }
uint8_t net_peer_count() { return peer_count; }
bool lockstep_in_battle() { return battle_up; }
bool lockstep_cat_is_mine(uint64_t id, bool& mine) {
    if (resume_owners && session_cat_owner(id)>=0) {
        mine = session_cat_owner(id)==peer_position;
        if (split_flipped) mine = !mine;   // a split that contradicts the notes
        return true;
    }
    if (!known_owners || id < 1 || id > 4) return false;
    mine = id == 2; return true;
}
// The settlement's ownership source: the session note table, filled by the
// roster install / journal import WITHOUT any battle.
bool lockstep_owner_pos(uint64_t id, uint8_t& owner_pos) {
    if (!notes_ready && !resume_owners) return false;
    const int p=session_cat_owner(id);if(p>=0){owner_pos=(uint8_t)p;return true;}
    return false;
}
void lockstep_note_owner(uint64_t, uint8_t) {}
bool lockstep_live_health(uint64_t, int32_t&, int32_t&) { return false; }
bool roster_add_ids(const uint64_t*, uint32_t, const char*) { return true; }
bool roster_setup_replace_party(const uint64_t* ids, uint32_t count, const char*) {
    if (!replace_party_for_real) return true;                       // the real one writes the run's party vector
    static uint64_t written[4]; for (uint32_t i = 0; i < count && i < 4; ++i) written[i] = ids[i];
    put(director, kDir_CatIdCount, count); put(director, kDir_CatIdData, (const uint64_t*)written);
    return true;
}
void roster_setup_set_ready(bool) {}
bool roster_add_familiars(const uint64_t*, uint32_t, const char*) { return true; }
uint64_t* rng_global_stream() { return rng; }
void log_line(const char* tag, const char* fmt, ...) { if (!std::getenv("MGMP_TEST_LOG")) return; va_list a; va_start(a, fmt); std::printf("[%s] ", tag); std::vprintf(fmt, a); std::putchar(10); va_end(a); }
void log_line_lvl(LogLevel, const char* tag, const char* fmt, ...) { if (!std::getenv("MGMP_TEST_LOG")) return; va_list a; va_start(a, fmt); std::printf("[%s] ", tag); std::vprintf(fmt, a); std::putchar(10); va_end(a); }
bool net_send_catdata(const CatDataMsg& m) {
    published.push_back(m.id);
    if (m.size == sizeof(CatImage)) std::memcpy(&published_images[m.id], m.data, sizeof(CatImage));
    return true;
}
bool net_send_catdata_to(uint8_t peer, const CatDataMsg& m) {
    published.push_back(m.id); published_to.push_back(peer);
    if (m.size == sizeof(CatImage)) std::memcpy(&published_images[m.id], m.data, sizeof(CatImage));
    return true;
}
bool net_send_catdigest(const CatDigestMsg& m) { digests.push_back(m); return true; }
uintptr_t addr_of_call(Call c) {
    switch (c) {
    case C_SerializeCatData: return (uintptr_t)&serialize;
    case C_ByteStreamDtor: return (uintptr_t)&bs_dtor;
    case C_OfstreamCtor: return (uintptr_t)&ofstream_ctor;
    case C_CatDataById: return (uintptr_t)&by_id;
    case C_CatAlloc: return (uintptr_t)&allocate;
    case C_CatCtor: return (uintptr_t)&construct;
    case C_CatPostLoad: return (uintptr_t)&post_load;
    case C_CatParents: return (uintptr_t)&parents;
    case C_CatIdAppend: return (uintptr_t)&append;
    case C_CatStatusRangeDtor: return (uintptr_t)&destroy_status_range;
    default: return 0;
    }
}
uintptr_t addr_of_data(DataSym d) {
    if (d == D_MewDirectorPtr) return (uintptr_t)&director_ptr;
    if (d == D_CatIdCounter) return (uintptr_t)&counter;
    if (d == D_CatRegister) return (uintptr_t)&register_cat;
    return 0;
}
}
int main() {
    wide_names(); initialize(); imports_and_membership(); entry_replacement(); rejected_inputs();
    local_upgrade_after_peer_push(); optional_item_replacement(); resume_owned_snapshots(); settlement_owner_filter();
    settlement_without_battle_split();
    in_session_next_run_replaces_stale_session_cats(); replace_party_for_real = true; clones_return_to_their_originals();
    legacy_clones_are_merged_before_they_are_overwritten(); merge_refusals_and_rollback(); clones_without_an_original_and_the_id_counter(); copies_at_home_do_not_make_the_original_ambiguous(); stuck_originals_are_released(); replace_party_for_real = false; variable_exports_and_settlement();
    digest_heals_from_owner(); catsync_shutdown();
    auto* ids = (IdVector*)(director + kDir_CatFamiliars);
    std::free(ids->data);
    for (void* p : allocated) {
        auto& v = statuses(p);
        destroy_status_range(v.begin, v.end);
        std::free(v.begin);
        std::free(p);
    }
    std::printf("test_catsync: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
